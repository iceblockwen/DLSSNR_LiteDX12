// The panel's ImGui window: the three tabs and the resource previews.
#include "panel_impl.hpp"
#include "ui_widgets.hpp"
#include "../input/input.hpp"
#include "../input/input_keyboard.hpp"
#include "../graphics_registry.hpp"
#include "../config.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
using namespace litedx12::panel_ui;
namespace litedx12
{
using panel_detail::Check;
void Panel::Impl::BuildResourceUi(float scale)
{
    const auto resources = nr::PreviewSnapshot();
    const bool wide = ImGui::GetContentRegionAvail().x >= 450 * scale;
    if (ImGui::BeginTable("All resource previews", wide ? 3 : 1,
                          ImGuiTableFlags_SizingStretchSame))
    {
        const char* labels[]{"Color", "Depth", "Motion vectors"};
        for (unsigned n = 0; n < 3; ++n)
        {
            ImGui::TableNextColumn();
            ImGui::PushID(int(n));
            ImGui::Separator();
            ImGui::TextUnformatted(labels[n]);
            const auto r = resources.resources[n];
            if (r.present)
            {
                ImGui::TextWrapped("%u x %u / DXGI %u", r.width, r.height, r.format);
            }
            else
            {
                ImGui::TextDisabled("Missing");
            }
            if (previewTexture && previewSelected && previewWidth && previewHeight &&
                r.present)
            {
                float w = (std::max)(1.f, ImGui::GetContentRegionAvail().x);
                float h =
                    (std::min)(180 * scale, w * float(previewHeight) / float(previewWidth / 3));
                w = h * float(previewWidth / 3) / float(previewHeight);
                const float tileWidth = float(previewWidth / 3), atlasWidth = float(previewWidth);
                ImGui::Image(
                    previewTexture, ImVec2(w, h),
                    ImVec2((n * tileWidth + .5f) / atlasWidth, .5f / previewHeight),
                    ImVec2(((n + 1) * tileWidth - .5f) / atlasWidth, 1 - .5f / previewHeight));
            }
            else
            {
                ImGui::TextDisabled("Preview unavailable");
            }
            if (n == 0)
            {
                ItemTip("Display-range color preview.");
            }
            if (n == 1)
            {
                bool contrast = nr::DepthContrast();
                if (ImGui::Checkbox("Depth contrast", &contrast))
                {
                    nr::SetDepthContrast(contrast);
                }
                ItemTip(
                    "Increase depth-preview contrast to make near/far differences easier to see. Does not change model depth. Default: off.");
            }
            if (n == 2)
            {
                float value = nr::DisplayScale();
                ImGui::SetNextItemWidth(
                    (std::max)(80 * scale, ImGui::GetContentRegionAvail().x - 70 * scale));
                bool finished = false;
                if (PanelSliderFloat("Scale", &value, .001f, 64.f, "%.3g",
                                     ImGuiSliderFlags_Logarithmic, &finished))
                {
                    nr::SetDisplayScale(value, false);
                    scaleEdited = true;
                }
                if (finished && scaleEdited)
                {
                    nr::SetDisplayScale(value, true);
                    scaleEdited = false;
                }
                if (ImGui::SmallButton("Reset scale"))
                {
                    nr::SetDisplayScale(1);
                }
                ItemTip("Restore motion-vector preview scale to 1.");
                ImGui::TextWrapped("MV scale: %.4g, %.4g", resources.motionScaleX,
                                   resources.motionScaleY);
                ItemTip("Red +X, green +Y, gray zero. Scale changes preview only.");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void Panel::Impl::BuildPipelineStatus(const nr::ModelSettings& settings, const nr::PassStatus& pass,
                                      const nr::ModelStatus& model)
{
    if (!ImGui::CollapsingHeader("Current pipeline", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }
    const ImVec4 green(.35f, .9f, .55f, 1), amber(1, .78f, .3f, 1), muted(.7f, .7f, .7f, 1);
    const auto now = GetTickCount64();
    const auto dlss = nr::GameDlssSnapshot();
    const bool recentDlss = dlss.observed && now - dlss.lastEvaluation <= 2000;
    const bool activeDlss = recentDlss && !dlss.released && dlss.result == 1;
    const char* dlssText = !dlss.observed     ? "Not observed"
                           : dlss.released    ? "Last observed SR feature released"
                           : !recentDlss      ? "No recent SR evaluation"
                           : dlss.result != 1 ? "Last SR evaluation failed"
                                              : "Active";
    ImGui::TextColored(activeDlss ? green : amber, "Game DLSS / DLAA: %s", dlssText);
    ItemTip(
        "Based on actual game NGX SR evaluation, including while NR is off. No recent evaluation can mean DLSS is off, a menu/pause, or an unobserved route; it is not proof that the game setting is off.");
    const bool currentInput = pass.hasInput && now - pass.inputTime <= 2000;
    const bool running = settings.enabled && currentInput && pass.ready && pass.actualPasses;
    // The pipeline owns the wait-reason writer; the panel only displays this text.
    const char* waitText =
        activeDlss
            ? "Game DLSS/DLAA observed, but no NR input captured"
        : !dlss.observed
            ? "No game NGX SR evaluation observed; waiting for DLSS/DLAA input"
            : "No recent successful game NGX SR evaluation; waiting for current DLSS/DLAA input";
    const bool failed =
        FAILED(model.error) || pass.failedPass || (FAILED(pass.error) && !pass.frameSkipped);
    ImGui::TextColored(!settings.enabled ? muted
                       : running         ? green
                                         : amber,
                       "NR: %s",
                       !settings.enabled ? "Off"
                       : failed          ? "Failed; toggle NR off/on to retry"
                       : !currentInput   ? "Waiting for current input"
                       : running         ? "Processing"
                                         : "Original frame / waiting");
    if (settings.enabled && !failed)
    {
        if (!pass.hasInput)
        {
            ImGui::TextWrapped("%s", waitText);
        }
        else if (model.state != State::Ready)
        {
            ImGui::TextWrapped("%s", Utf8(model.reason).c_str());
        }
        else
        {
            ImGui::TextWrapped("%s", pass.reason);
        }
    }
    if (!currentInput)
    {
        ImGui::TextDisabled("NR inputs: no recent input received");
        return;
    }
    // Every NR input is the game's own post-DLSS evaluate.
    ImGui::TextWrapped("Last NR color: game post-DLSS output%s",
                       pass.sceneLinear ? " + white normalization / encoding"
                                        : " (display domain; non-HDR bypass: no exposure/encoding)");
    ImGui::TextWrapped("Depth: game native%s",
                       pass.inputConverted[1] ? " + format conversion" : "");
    ImGui::PushStyleColor(ImGuiCol_Text, green);
    ImGui::TextWrapped("MV: game native%s | scale %.5g, %.5g",
                       pass.inputConverted[2] ? " + format conversion" : "",
                       double(pass.motionScaleX), double(pass.motionScaleY));
    ImGui::PopStyleColor();
    ItemTip(
        "Origin and required adapters are taken from the NR input path, not from the Resources preview. DX12 always uses the game's own NGX motion vectors, so copying or sharing an unchanged resource never introduces them. Model scale values are shown separately.");
}

void Panel::Impl::BuildUi()
{
    bool resourcesActive = false;
    const auto display = ImGui::GetIO().DisplaySize;
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (display.x != layoutDisplay.x || display.y != layoutDisplay.y || scale != layoutScale)
    {
        repairPosition = true;
    }
    layoutDisplay = display;
    layoutScale = scale;
    ImGui::SetNextWindowSize(ImVec2((std::min)(540 * scale, (std::max)(300.f, display.x - 56)),
                                    (std::min)(560 * scale, (std::max)(240.f, display.y - 56))),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(28, 28), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(200, 180),
        ImVec2((std::max)(200.f, display.x - 56), (std::max)(180.f, display.y - 56)));
    // Keep the existing ImGui ID while updating the visible product name.
    if (ImGui::Begin("DLSSNR_LiteDX12###Lite", &status.visible, ImGuiWindowFlags_NoCollapse))
    {
        // Repair after viewport/DPI changes, never fight a user's active drag.
        if (repairPosition && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const auto size = ImGui::GetWindowSize();
            const auto pos = ImGui::GetWindowPos();
            const ImVec2 bounded((std::clamp)(pos.x, 0.f, (std::max)(0.f, display.x - size.x)),
                                 (std::clamp)(pos.y, 0.f, (std::max)(0.f, display.y - size.y)));
            if (bounded.x != pos.x || bounded.y != pos.y)
            {
                ImGui::SetWindowPos(bounded);
            }
            repairPosition = false;
        }
        if (ImGui::BeginTabBar("LitePages"))
        {
            if (ImGui::BeginTabItem("Overview"))
            {
                ImGui::BeginChild("Overview contents", ImVec2(0, 0));
                ImGui::TextColored(ImVec4(1, .55f, .2f, 1), "DLSSNR_LiteDX12");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", LITE_VERSION);
                ImGui::Separator();
                ImGui::TextWrapped("%s | %u x %u", "DirectX 12",
                                   status.width, status.height);
                ImGui::Spacing();
                ImGui::Separator();
                auto modelSettings = nr::GetModelSettings();
                bool modelChanged = false;
                modelChanged |= ImGui::Checkbox("Neural rendering", &modelSettings.enabled);
                ItemTip(
                    "Enable NR processing. Requires a supported NVIDIA device and the NR model. Default: off.");
                const auto model = nr::Runtime().Snapshot();
                const auto pass = nr::LastPass();
                BuildPipelineStatus(modelSettings, pass, model);
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Model parameters",
                                            ImGuiTreeNodeFlags_DefaultOpen))
                {
                    int preset = int(modelSettings.pass.preset),
                        style = int(modelSettings.pass.style);
                    if (ImGui::Combo("Model preset", &preset,
                                     "Default (model choice)\0A\0B\0"))
                    {
                        modelSettings.pass.preset = unsigned(preset);
                        modelChanged = true;
                    }
                    ItemTip(
                        "A: original model. B: optimized model (requires 310.9.16.0 or newer). Default lets the DLL choose: B in 310.9.16.0; the older DLL falls back to its original model. Changing preset rebuilds NR histories.");
                    if (ImGui::Combo("Style", &style,
                                     "Default (standard)\0Natural\0Cinematic\0"))
                    {
                        modelSettings.pass.style = unsigned(style);
                        modelChanged = true;
                    }
                    ItemTip(
                        "Choose the model's visual style. Changing it rebuilds NR histories. Default: standard.");
                    auto slider = [&](const char* label, float& value, float low, float high,
                                      float initial)
                    {
                        SliderResetWidth(label);
                        modelChanged |= PanelSliderFloat(label, &value, low, high, "%.3f");
                        if (std::strcmp(label, "Difference gain") == 0)
                        {
                            ItemTip("1: normal NR | 0: original | 2: double the NR difference. Applied once after NR.");
                        }
                        ImGui::SameLine();
                        ImGui::PushID(label);
                        modelChanged |= ResetValue(value, initial);
                        ImGui::PopID();
                    };
                    auto tuningSlider = [&](unsigned n, const char* label, float& value,
                                            float low, float initial)
                    {
                        float editing = tuningPending[n] ? tuningValues[n] : value;
                        bool finished = false;
                        SliderResetWidth(label);
                        if (PanelSliderFloat(label, &editing, low, 2, "%.3f", 0, &finished))
                        {
                            tuningValues[n] = editing;
                            tuningPending[n] = true;
                        }
                        if (finished && tuningPending[n])
                        {
                            value = tuningValues[n];
                            tuningPending[n] = false;
                            modelChanged = true;
                        }
                        ImGui::SameLine();
                        ImGui::PushID(label);
                        if (ResetValue(value, initial))
                        {
                            tuningPending[n] = false;
                            modelChanged = true;
                        }
                        ImGui::PopID();
                    };
                    tuningSlider(0, "Intensity", modelSettings.pass.intensity, 0, 1);
                    tuningSlider(1, "Local structure", modelSettings.pass.structure, 0, 1);
                    tuningSlider(2, "Local tone", modelSettings.pass.tone, 0, 1);
                    tuningSlider(3, "Skin structure", modelSettings.pass.skin, -1, -1);
                    bool autoMask = modelSettings.pass.autoMask != 0;
                    if (ImGui::Checkbox("AutoMask", &autoMask))
                    {
                        modelSettings.pass.autoMask = autoMask ? 1u : 0u;
                        modelChanged = true;
                    }
                    ItemTip(
                        "Let NR derive its own application mask. Nothing is bound as DLSSNR.ControlMask. Changing this rebuilds histories. Default: on.");
                    bool uiCorrection = modelSettings.pass.uiCorrection != 0;
                    if (ImGui::Checkbox("UI correction", &uiCorrection))
                    {
                        modelSettings.pass.uiCorrection = uiCorrection ? 1u : 0u;
                        modelChanged = true;
                    }
                    ItemTip(
                        "Pass the model's UI correction hint (DLSSNR.UICorrection). DLSSNR_LiteDX12 still captures after upscale, before or after the game's UI depending on the game. Changing this rebuilds histories. Default: off.");
                    slider("Difference gain", modelSettings.pass.differenceGain, 0, 2, 1);
                    modelChanged |= ImGui::Checkbox("Preserve HDR highlights", &modelSettings.pass.composed);
                    ItemTip("Compose the model result with the original input; off applies the model output directly. Switching resets NR history.");
                }
                else
                {
                    modelChanged |= CommitTuning(&modelSettings);
                }
                if (modelChanged)
                {
                    nr::SetModelSettings(modelSettings);
                }
                if (ImGui::CollapsingHeader("GPU timing / errors"))
                {
                    bool forceTiming = litedx12::ForceTiming();
                    if (ImGui::Checkbox("Force GPU timing (panel closed)", &forceTiming))
                    {
                        nr::SetForceTimingPersisted(forceTiming);
                    }
                    ItemTip(
                        "Runs the timestamp path even while the panel is hidden, so the two figures "
                        "below keep updating. Games that resolve NR into their own colour need it: "
                        "without the GPU-side sync that the timestamp queries force, only part of the "
                        "frame keeps NR. Costs four timestamps and one query resolve per frame; the "
                        "readback never blocks the CPU. Off by default.");
                    if (pass.timingValid)
                    {
                        ImGui::TextWrapped("Completed NR GPU: %.3f ms (model %.3f ms)",
                                           double(pass.gpuMilliseconds),
                                           double(pass.modelMilliseconds));
                        ItemTip(
                            "Shows the last completed NR recording while the panel is open: guide conversion, white meter, encode, model and resolve. Model is the NGX evaluate span. Excludes NGX feature creation and the game's own queue work.");
                    }
                    if (FAILED(model.error))
                    {
                        ImGui::Text("Runtime HRESULT 0x%08lX | NGX 0x%08X",
                                    static_cast<unsigned long>(model.error), model.ngxResult);
                    }
                }
                const auto config = ConfigStorageSnapshot();
                if (config.error || config.fallbackCause)
                {
                    if (config.error)
                    {
                        ImGui::TextWrapped("Configuration save failed (Windows error %lu).",
                                           config.error);
                    }
                    else
                    {
                        ImGui::TextWrapped("Configuration fallback (Windows error %lu).",
                                           config.fallbackCause);
                    }
                    ItemTip(
                        "Saves run in the background. If the game directory blocks writing, a per-game profile is used. Failed saves leave parameters active in memory.");
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Insert: show / hide panel");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            else
            {
                CommitTuning();
            }
            if (ImGui::BeginTabItem("White point"))
            {
                ImGui::BeginChild("White point contents", ImVec2(0, 0));
                auto modelSettings = nr::GetModelSettings();
                bool modelChanged = false;
                const auto pass = nr::LastPass();
                if (!modelSettings.enabled)
                {
                    ImGui::TextDisabled(
                        "NR is off; measurements wait until NR processing is active.");
                }
                int whiteMode = int(modelSettings.pass.white.mode);
                if (ImGui::Combo("White point", &whiteMode,
                                 "Game exposure\0Measured input\0Manual\0"))
                {
                    modelSettings.pass.white.mode = nr::WhiteMode(whiteMode);
                    modelChanged = true;
                }
                ItemTip(
                    "Game exposure uses the game's exposure inputs. Measured input estimates white from the original image and applies the filter below. Manual uses a fixed value. Display input bypasses normalization. Default: Game exposure.");
                if (whiteMode == 0)
                {
                    int ratio = modelSettings.pass.white.reverseRatio ? 1 : 0;
                    if (ImGui::Combo("Exposure ratio", &ratio,
                                     "Pre-exposure / exposure\0Exposure / pre-exposure\0"))
                    {
                        modelSettings.pass.white.reverseRatio = ratio != 0;
                        modelChanged = true;
                    }
                    ItemTip(
                        "Choose which side of the game's exposure pair the white divisor takes. Both directions are evaluated live on the GPU from the exposure texture; the texture value is never inverted, only its position in the fraction.");
                    modelChanged |= ImGui::Checkbox("Use pre-exposure",
                                                    &modelSettings.pass.white.usePreExposure);
                    ItemTip("Use DLSS.Pre.Exposure. Unchecked substitutes 1. Default: on.");
                    modelChanged |= ImGui::Checkbox("Use exposure scale",
                                                    &modelSettings.pass.white.useExposureScale);
                    ItemTip("Use DLSS.Exposure.Scale on the scalar path. Unchecked substitutes 1. A supplied, enabled exposure texture keeps its existing priority.");
                    modelChanged |= ImGui::Checkbox("Read game's exposure texture",
                                                    &modelSettings.pass.white.useTexture);
                    ItemTip(
                        "Use a supplied 1 x 1 game exposure texture. Off selects the scalar path; unchecked scalar inputs are 1. For texture only, leave this checked and uncheck both scalar inputs. Default: on.");
                }
                const nr::WhiteSettings defaults;
                auto whiteSlider = [&](const char* label, float& value, float low, float high,
                                       float initial, const char* format = "%.3f",
                                       ImGuiSliderFlags flags = 0)
                {
                    SliderResetWidth(label);
                    modelChanged |= PanelSliderFloat(label, &value, low, high, format, flags);
                    ImGui::SameLine();
                    ImGui::PushID(label);
                    modelChanged |= ResetValue(value, initial);
                    ImGui::PopID();
                };
                whiteSlider("Manual / fallback", modelSettings.pass.white.manual, 1e-8f, 1024.f,
                            defaults.manual, "%.9g", ImGuiSliderFlags_Logarithmic);
                whiteSlider("White trim", modelSettings.pass.white.trim, .001f, 1024.f,
                            defaults.trim, "%.6g", ImGuiSliderFlags_Logarithmic);
                if (whiteMode == 0)
                {
                    whiteSlider("Game exponent", modelSettings.pass.white.exponent, .1f, 4.f,
                                defaults.exponent, "%.3f");
                    ItemTip(
                        "Raises the game exposure ratio to this power before the trim. 1 is the plain linear mapping. Below 1 compresses the range so one trim covers both bright and dark scenes; above 1 expands it. Only affects the game exposure white point.");
                }
                ImGui::SeparatorText("Current white point");
                const ImVec4 green(.35f, .9f, .55f, 1), blue(.45f, .75f, 1, 1),
                    amber(1, .78f, .3f, 1), muted(.7f, .7f, .7f, 1);
                const bool liveWhite = pass.white.source == nr::WhiteSource::Game &&
                                       pass.gameExposureTexture &&
                                       modelSettings.pass.white.useTexture;
                if (!modelSettings.enabled || !pass.hasInput)
                {
                    ImGui::TextColored(muted, "Current white: -- (%s)",
                                       modelSettings.enabled ? "waiting for input" : "NR off");
                }
                else if (!pass.sceneLinear)
                {
                    ImGui::TextColored(blue, "Current white: 1 (display input bypass)");
                }
                else if (liveWhite)
                {
                    ImGui::TextColored(green, "Current white: live game exposure (GPU)%s",
                                       pass.gpuDivisorValid ? "" : " / awaiting sample");
                    if (pass.gpuDivisorValid)
                    {
                        ImGui::TextColored(blue, "Last completed white: %.6g",
                                           double(pass.gpuDivisor));
                    }
                }
                else
                {
                    const bool fallback = !pass.white.valid ||
                                          pass.white.source == nr::WhiteSource::Waiting ||
                                          pass.white.source == nr::WhiteSource::Held;
                    const bool manual = pass.white.source == nr::WhiteSource::Manual;
                    ImGui::TextColored(fallback ? amber
                                       : manual ? blue
                                                : green,
                                       "Current white: %.6g (%s)", double(pass.white.value),
                                       fallback ? "fallback / held"
                                       : manual ? "manual"
                                       : pass.white.source == nr::WhiteSource::Game
                                           ? "game exposure"
                                           : "measured input");
                }
                ItemTip(
                    "GPU-live exposure reads the texture on the GPU; the sample is the last completed readback. Display input uses divisor 1. Fallback uses manual white, or holds the last valid value.");
                ImGui::SeparatorText("White point status");
                ImGui::TextWrapped("Input: %s",
                                   !pass.hasInput ? "Not received"
                                   : pass.sceneLinear
                                       ? "Scene linear; white normalization and encoding"
                                       : "Display referred; white normalization bypassed");
                if (pass.gpuExposureValid)
                {
                    ImGui::Text("Last sampled game exposure: %.5g", double(pass.gpuExposure));
                }
                else
                {
                    ImGui::TextDisabled(
                        "Game exposure texture value: unavailable / not measured");
                }
                if (pass.preExposurePresent)
                {
                    ImGui::Text("Game pre-exposure: %.5g", double(pass.preExposure));
                }
                else
                {
                    ImGui::TextDisabled("Game pre-exposure: not supplied");
                }
                if (pass.exposureScalePresent)
                {
                    ImGui::Text("Game exposure scale: %.5g", double(pass.exposureScale));
                }
                else
                {
                    ImGui::TextDisabled("Game exposure scale: not supplied");
                }
                ImGui::SeparatorText("Original input statistics");
                if (FAILED(pass.whiteMeasurementError))
                {
                    ImGui::TextWrapped(
                        "White measurement failed (0x%08lX); manual/history fallback.",
                        static_cast<unsigned long>(pass.whiteMeasurementError));
                }
                if (pass.white.statisticsValid)
                {
                    ImGui::Text("Mean %.5g | log-average %.5g", double(pass.white.mean),
                                double(pass.white.logAverage));
                    ImGui::Text("p50 %.5g | p90 %.5g", double(pass.white.p50),
                                double(pass.white.p90));
                    ImGui::Text("p99 %.5g | peak %.5g", double(pass.white.p99),
                                double(pass.white.peak));
                }
                if (ImGui::CollapsingHeader("White point filter",
                                            ImGuiTreeNodeFlags_DefaultOpen))
                {
                    // These five only drive the Measured branch of WhitePoint::Resolve.
                    if (whiteMode == 1)
                    {
                        whiteSlider("Mid-gray anchor", modelSettings.pass.white.anchor, .02f, 1.f,
                                    defaults.anchor);
                        whiteSlider("Filter seconds", modelSettings.pass.white.tau, .001f, 60.f,
                                    defaults.tau, "%.3g", ImGuiSliderFlags_Logarithmic);
                        whiteSlider("Deadband", modelSettings.pass.white.deadband, 0, 1.f,
                                    defaults.deadband);
                        whiteSlider("Attack (stops/s)", modelSettings.pass.white.attack, 0, 16.f,
                                    defaults.attack, "%.2f");
                        whiteSlider("Release (stops/s)", modelSettings.pass.white.release, 0, 16.f,
                                    defaults.release, "%.2f");
                    }
                    SliderResetWidth("Measure every N frames");
                    int every = int(modelSettings.pass.white.every);
                    if (PanelSliderInt("Measure every N frames", &every, 1, 120))
                    {
                        modelSettings.pass.white.every = unsigned(every);
                        modelChanged = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset##WhiteEvery"))
                    {
                        modelSettings.pass.white.every = defaults.every;
                        modelChanged = true;
                    }
                    ItemTip("Restore the measurement interval to 4 processed NR frames.");
                }
                if (modelChanged)
                {
                    nr::SetModelSettings(modelSettings);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Resources"))
            {
                resourcesActive = true;
                if (!nr::PreviewSelected())
                {
                    nr::SetPreviewSelected(true);
                }
                if (ImGui::BeginChild("Resource contents", ImVec2(0, 0)))
                {
                    BuildResourceUi(scale);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    // Closing keeps the selected tab/view for reopening. A visible panel
    // on another page has no extra preview demand and never persists it.
    if (status.visible && !resourcesActive && nr::PreviewSelected())
    {
        nr::SetPreviewSelected(false);
    }
    SyncPreviewVisibility();
    if (!status.visible)
    {
        CapturePanelInput(hwnd, false);
    }
}

}
