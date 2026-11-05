// SPDX-License-Identifier: GPL-3.0-or-later
#include "pass.hpp"
#include "lite_color_shader.hpp"
#include "../graphics.hpp"
#include <cstdio>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <vector>
#include <atomic>
namespace litedx12::nr {
namespace {
constexpr auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
bool Plain(const D3D12_RESOURCE_DESC &d) {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
           d.MipLevels == 1 && d.SampleDesc.Count == 1 && d.Width && d.Height;
}
void Resource(FeatureBackend &backend, const char *key, ID3D12Resource *r) {
    backend.SetResource(key, r);
}
// QueryPerformance calls never appear in the steady-state path.
// Settings arrive validated by SetModelSettings; only game-provided values are bounded here.
void Tuning(FeatureBackend &backend, const PassSettings &s) {
    backend.SetUnsigned("DLSSNR.Hint.Render.Preset", s.preset);
    backend.SetUnsigned("DLSSNR.Style", s.style);
    backend.SetFloat("DLSSNR.Intensity", s.intensity);
    backend.SetFloat("DLSSNR.LocalStructureStrength", s.structure);
    backend.SetFloat("DLSSNR.LocalToneStrength", s.tone);
    backend.SetFloat("DLSSNR.SkinStructureStrength", s.skin);
    backend.SetUnsigned("DLSSNR.UseAutoMask", s.autoMask ? 1u : 0u);
    backend.SetUnsigned("DLSSNR.UICorrection", s.uiCorrection ? 1u : 0u);
}
void Region(FeatureBackend &backend, const char *prefix, unsigned w, unsigned h, unsigned x = 0,
            unsigned y = 0) {
    // Runs for every recording: build the keys on the stack, never on the heap.
    constexpr const char *suffix[]{"SubrectBaseX", "SubrectBaseY", "SubrectWidth", "SubrectHeight"};
    const unsigned value[]{x, y, w, h};
    for (unsigned n = 0; n < 4; ++n) {
        char key[64]{};
        snprintf(key, sizeof(key), "DLSSNR.%s%s", prefix, suffix[n]);
        backend.SetUnsigned(key, value[n]);
    }
}
ComPtr<ID3D12Resource> Texture(ID3D12Device *d, unsigned w, unsigned h, DXGI_FORMAT f) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = f;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    ThrowIfFailed(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, UAV, nullptr,
                                             IID_PPV_ARGS(&r)));
    return r;
}
// A model feature whose recording may still be in flight is parked rather than released:
// freeing one under work the game's own queue still references removes the device. OptiScaler
// parks for a fixed 32 evaluates because it has no fence to ask. Tickets cover a
// single submission; replaying a closed list is not covered by this retirement rule.
struct ParkedFeature {
    void *feature{};
    FeatureBackend *backend{};
    std::shared_ptr<GpuTicket> ticket;
};
std::mutex parkedMutex;
std::vector<ParkedFeature> parkedFeatures;
std::atomic<size_t> parkedCount{};
void ParkFeature(void *feature, FeatureBackend *backend, const std::shared_ptr<GpuTicket> &ticket) {
    if (!feature || !backend)
        return;
    std::lock_guard lock(parkedMutex);
    parkedFeatures.push_back({feature, backend, ticket});
    parkedCount=parkedFeatures.size();
}
void DrainParkedFeatures() {
    std::vector<ParkedFeature> released;
    {
        std::lock_guard lock(parkedMutex);
        for (auto it = parkedFeatures.begin(); it != parkedFeatures.end();) {
            const auto state = it->ticket ? it->ticket->Status() : GpuTicket::State::Discarded;
            const bool safe = state == GpuTicket::State::Discarded ||
                              (state == GpuTicket::State::Submitted && it->ticket->Complete());
            if (!safe) {
                ++it;
                continue;
            }
            released.push_back(*it);
            it = parkedFeatures.erase(it);
        }
        parkedCount=parkedFeatures.size();
    }
    // Release outside the lock: this calls into the model, not into the container.
    for (auto &parked : released)
        parked.backend->Release(parked.feature);
}
struct Layer {
    FeatureBackend *backend{};
    void *feature{};
    std::shared_ptr<GpuTicket> last;
    bool first = true, failed{};
    ~Layer() {
        // Never release under work that may still be in flight, and never drop the handle
        // either: park it and let its ticket's fence decide. A pending or discarded
        // initialization is what used to lose this handle for good.
        ParkFeature(feature, backend, last);
        feature = nullptr;
    }
};
struct Context {
    unsigned w{}, h{};
    PassSettings tuning;
    DXGI_FORMAT format{};
    ComPtr<ID3D12Resource> proxy, answer, output;
    ComPtr<ID3D12Resource> guides[2];
    std::shared_ptr<Layer> layer;
    bool failed{}, outputReadable{};
};
struct TimingResources {
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> times;
    explicit TimingResources(ID3D12Device *device) {
        D3D12_QUERY_HEAP_DESC q{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
        ThrowIfFailed(device->CreateQueryHeap(&q, IID_PPV_ARGS(&timestamps)));
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 4 * sizeof(UINT64);
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&times)));
    }
};
struct Recording {
    std::shared_ptr<Context> context;
    // Tuning can replace Context::layer while old GPU recordings still use it.
    std::shared_ptr<Layer> layer;
    PassFrame input;
    ComPtr<ID3D12Resource> exposure;
    ResourceMetadata exposureMetadata{};
    ComPtr<ID3D12Resource> modelGuides[2];
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::shared_ptr<WhiteReadback> meter;
    std::shared_ptr<TimingResources> timing;
    unsigned timedLayers{};
    bool evaluated{}, createdFeature{};
    UINT64 whiteEpoch{};
    bool sceneLinear{}, hasExposure{}, modelMeasurement{};
    unsigned liveExposure{}; // 0 off, 1 pre/exposure, 2 exposure/pre
    float preMultiplier = 1, fallbackWhite = 1;
    // Game-exposure ratio mapping used by the live GPU white path.
    float whiteExponent = 1, whiteTrim = 1;
    void RetireInputs() {
        if (input.ticket->Complete()) {
            input.resources = {};
            input.resolveTarget.Reset();
            for (auto &guide : modelGuides)
                guide.Reset();
            input.exposureTexture.Reset();
            exposure.Reset();
            if (meter)
                meter->RetireInputs();
        }
    }
    void Timing(std::shared_ptr<TimingResources> resources) {
        if (!resources)
            return;
        timing = std::move(resources);
        input.ticket->RequestTiming();
    }
    void ReadTiming(PassStatus &status) {        const auto frequency = input.ticket->TimestampFrequency();
        if (!timing || !frequency || !input.ticket->Complete())
            return;
        auto *times = timing->times.Get();
        void *data{};
        D3D12_RANGE range{0, 4 * sizeof(UINT64)};
        if (FAILED(times->Map(0, &range, &data)))
            return;
        auto *ticks = static_cast<UINT64 *>(data);
        bool valid = ticks[1] >= ticks[0];
        for (unsigned n = 0; n < timedLayers; ++n)
            valid = valid && ticks[3 + 2 * n] >= ticks[2 + 2 * n];
        if (valid) {
            status.gpuMilliseconds = float(double(ticks[1] - ticks[0]) * 1000 / double(frequency));
            status.modelMilliseconds =
                timedLayers ? float(double(ticks[3] - ticks[2]) * 1000 / double(frequency)) : 0;
            status.timingValid = true;
        }
        D3D12_RANGE noWrite{};
        times->Unmap(0, &noWrite);
    }
};
bool SameTuning(const PassSettings &a, const PassSettings &b) {
    return a.preset == b.preset && a.style == b.style && a.autoMask == b.autoMask &&
           a.uiCorrection == b.uiCorrection && a.intensity == b.intensity &&
           a.structure == b.structure && a.tone == b.tone && a.skin == b.skin;
}
}
bool LinearColorFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
           f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R32G32B32A32_TYPELESS ||
           f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R32G32B32_FLOAT;
}
bool HasParkedModelFeatures() {return parkedCount.load()!=0;}
void RetireModelFeatures() {DrainParkedFeatures();}
struct PassExecutor12::Impl {
    ComPtr<ID3D12Device> device;
    FeatureBackend &backend;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::unique_ptr<WhiteMeter12> meter;
    WhitePoint white;
    std::shared_ptr<Context> context;
    std::shared_ptr<Recording> previous;
    std::vector<std::shared_ptr<Recording>> inFlight;
    std::vector<std::shared_ptr<TimingResources>> timingPool;
    mutable std::mutex mutex;
    PassStatus status;
    UINT64 tick{}, frameIndex{};
    UINT64 whiteEpoch{};
    bool initialized{}, attempted{};
    HRESULT initializationError = S_OK;
    bool domainKnown{}, linearDomain{};
    bool restoreKnown{}, composedRestore{};
    unsigned guideWidth[2]{}, guideHeight[2]{};
    Guides previousGuides;
    bool haveGuides{};
    // timing summary accumulators. Nothing here runs below Verbose.
    explicit Impl(ID3D12Device *d, FeatureBackend &b) : device(d), backend(b) {
    }
    std::shared_ptr<TimingResources> AcquireTiming() {
        // Recordings retain these through submission and fence retirement. Sole
        // pool ownership also excludes any caller still retaining a completed frame.
        for (auto &resources : timingPool)
            if (resources.use_count() == 1) {
                return resources;
            }
        if (timingPool.size() >= 8)
            return {}; // Optional diagnostics never wait for GPU.
        auto resources = std::make_shared<TimingResources>(device.Get());
        timingPool.push_back(resources);
        return resources;
    }
    void Shaders() {
        if (root && pipeline)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
                                         {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 4}};
        D3D12_ROOT_PARAMETER p[2]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[0].DescriptorTable = {2, ranges};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[1].Constants = {0, 0, 16};
        D3D12_ROOT_SIGNATURE_DESC desc{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> code, error;
        ThrowIfFailed(
            D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &code, &error));
        ThrowIfFailed(device->CreateRootSignature(0, code->GetBufferPointer(),
                                                  code->GetBufferSize(), IID_PPV_ARGS(&root)));
        // Bytecode is built by fxc at build time (CMakeLists): the render thread
        // never parses shader source.
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = root.Get();
        ps.CS = {liteColorShader, liteColorShaderSize};
        ThrowIfFailed(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline)));
    }
    HRESULT Record(ID3D12GraphicsCommandList *list, const PassFrame &frame, const Exposure &exposure,
                   const PassSettings &settings, bool reset,
                   std::shared_ptr<void> &keep, ID3D12Resource *&result) {
        result = frame.resources[0].Get();
        DrainParkedFeatures();
        status.edited = false;
        status.actualPasses = 0;
        status.frameSkipped = false;
        if (!settings.gpuTiming) {
            status.timingValid = false;
            // In-flight recordings retain their own queries until fence retirement.
            timingPool.clear();
        }
        ThrowIfFailed(device->GetDeviceRemovedReason());
        // A rejected frame never reached the model, so its temporal history is still valid:
        // nothing here may ask for a Reset on the next successful frame.
        auto refuse = [&](HRESULT hr, const char *why) {
            status.error = hr;
            status.reason = why;
            status.ready = false;
            status.frameSkipped = true;
            status.failedPass = 0;
            status.ngxResult = 0;
            return hr;
        };
        if (!list || !frame.ticket)
            return refuse(E_INVALIDARG, "Missing NR recording/submission contract");
        if (!frame.guides.motionResolutionKnown)
            return refuse(E_NOTIMPL,
                          "Missing NGX create flags; motion-vector resolution is unknown");
        if (!initialized) {
            if (!attempted) {
                attempted = true;
                initializationError = backend.Initialize(device.Get());
            }
            if (FAILED(initializationError)) {
                status.error = initializationError;
                status.frameSkipped = initializationError == E_NOTIMPL;
                status.reason = "NR runtime unavailable for this device; original frame preserved";
                return initializationError;
            }
            initialized = true;
        }
        status.error = S_OK;
        if (!backend.Capability()) {
            status.reason = "NGX capability block unavailable";
            return E_POINTER;
        }
        for (unsigned n = 0; n < 3; ++n) {
            auto *r = frame.resources[n].Get();
            const auto &metadata = frame.resourceMetadata[n];
            if (!r || !metadata.valid || !Plain(metadata.description))
                return refuse(E_NOTIMPL, "NR needs color, depth and motion textures");
            const auto &desc = metadata.description;
            auto &g = frame.guides;
            const auto w = g.width[n] ? g.width[n] : unsigned(desc.Width),
                       h = g.height[n] ? g.height[n] : desc.Height;
            if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                !ShaderViewCompatible(desc.Format, n == 1))
                return refuse(E_NOTIMPL, "NR input has no compatible shader view");
            const auto view = ViewFormat(desc.Format, n == 1);
            if (n == 2 && view != DXGI_FORMAT_R16G16_FLOAT && view != DXGI_FORMAT_R32G32_FLOAT &&
                view != DXGI_FORMAT_R16G16_SNORM)
                return refuse(E_NOTIMPL, "Unsupported motion-vector encoding");
            if (n == 1 && view != DXGI_FORMAT_R32_FLOAT && view != DXGI_FORMAT_R16_FLOAT &&
                view != DXGI_FORMAT_R16_UNORM && view != DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
                view != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
                return refuse(E_NOTIMPL, "Unsupported depth encoding");
            if (g.originX[n] > desc.Width || w > desc.Width - g.originX[n] ||
                g.originY[n] > desc.Height || h > desc.Height - g.originY[n])
                return refuse(E_INVALIDARG, "NR guide subrect exceeds its texture");
        }
        ComPtr<ID3D12Device> owner;
        ThrowIfFailed(list->GetDevice(IID_PPV_ARGS(&owner)));
        if (owner.Get() != device.Get() || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return refuse(E_INVALIDARG, "NR needs a direct command list from its device");
        if (!std::isfinite(frame.guides.motionScaleX) || !std::isfinite(frame.guides.motionScaleY))
            return refuse(E_INVALIDARG, "Invalid NR motion-vector scale");
        const auto &color = frame.resourceMetadata[0].description;
        const auto format = ViewFormat(color.Format);
        auto *resolveTarget = frame.resolveTarget.Get();
        if (resolveTarget) {
            const auto &targetMetadata = frame.resolveTargetMetadata;
            if (!targetMetadata.valid)
                return refuse(E_INVALIDARG, "Native NR resolve destination metadata missing");
            const auto &target = targetMetadata.description;
            if (!Plain(target) ||
                target.Width != color.Width || target.Height != color.Height ||
                target.Format != format ||
                !(target.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                return refuse(E_INVALIDARG, "Native NR resolve destination not compatible");
            for (const auto &input : frame.resources)
                if (resolveTarget == input.Get())
                    return refuse(E_INVALIDARG, "Native NR resolve needs immutable inputs");
        }
        status.hasInput = true;
        status.inputOrigin = frame.origin;
        status.inputTime = GetTickCount64();
        status.depthFlipped = frame.guides.flipDepth;
        status.motionScaleX = frame.guides.motionScaleX;
        status.motionScaleY = frame.guides.motionScaleY;
        for (unsigned n = 0; n < 3; ++n)
            status.inputConverted[n] = frame.converted[n];
        // These are the adapters required by this input. A skipped frame's
        // state/reason remains visible separately; it is not a successful run.
        status.inputConverted[1] |=
            frame.resourceMetadata[1].description.Format != DXGI_FORMAT_R32_FLOAT;
        status.inputConverted[2] |=
            frame.resourceMetadata[2].description.Format != DXGI_FORMAT_R16G16_FLOAT;
        status.sceneLinear = exposure.sceneLinear;
        status.preExposure = exposure.pre;
        status.exposureScale = exposure.scale;
        status.preExposurePresent = exposure.prePresent;
        status.exposureScalePresent = exposure.exposurePresent;
        auto canFollow = [&] {
            if (!(previous->evaluated || previous->createdFeature) ||
                !(frame.submissionDependencies && previous->input.ticket->CanOrderSubmission()) ||
                !context || context->failed)
                return false;
            if (context->w != color.Width || context->h != color.Height ||
                context->format != format || !SameTuning(context->tuning, settings))
                return false;
            // A successful creation may be followed after actual submission:
            // the ticket orders separate recordings, as in Opt's next-frame path.
            // Guide reallocation still requires retirement.
            if (!context->layer || !context->layer->feature || context->layer->failed)
                return false;
            for (unsigned n = 1; n < 3; ++n) {
                const auto &a = previous->input.resourceMetadata[n].description;
                const auto &b = frame.resourceMetadata[n].description;
                if (a.Width != b.Width || a.Height != b.Height || a.Format != b.Format)
                    return false;
            }
            return true;
        };
        if (previous) {
            if (previous->input.ticket->Status() == GpuTicket::State::Discarded) {
                // Drop the invalid history, not submitted recordings still owned by inFlight
                // or the caller. Feature lifetime remains owned by those recordings.
                previous.reset();
                context.reset();
                white.Reset();
                ++whiteEpoch;
                domainKnown = haveGuides = false;
                status.gpuExposureValid = status.gpuDivisorValid = false;
                status.readyPasses = status.failedPass = 0;
                status.timingValid = false;
                status.modelError = S_OK;
                return refuse(E_ABORT, "NR recording discarded; history rebuild on next frame");
            }
            // A tuning change rebuilds the feature anyway, and Layer::~Layer parks the old
            // handle instead of releasing it, so this rebuild must not wait for the previous
            // recording's fence. The ticket still orders this rebuild after the old work.
            const bool tuningRebuild = context && !SameTuning(context->tuning, settings) &&
                                       frame.submissionDependencies &&
                                       previous->input.ticket->CanOrderSubmission();
            if (!previous->input.ticket->Complete() && !canFollow() && !tuningRebuild) {
                if (context)
                    if (context->layer)
                        context->layer->first = true;
                status.ready = false;
                status.reason = "Waiting for NR creation, rebuild or unknown-queue recording";
                return S_FALSE;
            }
            if (!previous->input.ticket->Complete() && frame.submissionDependencies &&
                !frame.ticket->DependOn(previous->input.ticket))
                return refuse(E_ABORT,
                              "NR submission dependency unavailable; original frame preserved");
        }
        const UINT64 now = GetTickCount64();
        float dt = tick ? float(now - tick) * .001f : 0;
        tick = now;
        if (domainKnown && linearDomain != exposure.sceneLinear)
            reset = true;
        linearDomain = exposure.sceneLinear;
        domainKnown = true;
        if (restoreKnown && composedRestore != settings.composed)
            reset = true;
        restoreKnown = true;
        composedRestore = settings.composed;
        for (unsigned n = 0; n < 2; ++n) {
            const auto &d = frame.resourceMetadata[n + 1].description;
            if (guideWidth[n] && (guideWidth[n] != d.Width || guideHeight[n] != d.Height))
                reset = true;
            guideWidth[n] = unsigned(d.Width);
            guideHeight[n] = d.Height;
        }
        if (haveGuides) {
            if (previousGuides.flipDepth != frame.guides.flipDepth ||
                previousGuides.depthInverted != frame.guides.depthInverted ||
                previousGuides.motionScaleX != frame.guides.motionScaleX ||
                previousGuides.motionScaleY != frame.guides.motionScaleY)
                reset = true;
            for (unsigned n = 1; n < 3; ++n)
                if (previousGuides.originX[n] != frame.guides.originX[n] ||
                    previousGuides.originY[n] != frame.guides.originY[n] ||
                    previousGuides.width[n] != frame.guides.width[n] ||
                    previousGuides.height[n] != frame.guides.height[n])
                    reset = true;
        }
        previousGuides = frame.guides;
        haveGuides = true;
        if (reset) {
            white.Reset();
            ++whiteEpoch;
            status.gpuExposureValid = status.gpuDivisorValid = false;
        }
        white.Resolve(settings.white, exposure, 0);
        ComPtr<ID3D12DescriptorHeap> reusableHeap;
        auto retire = [&](const std::shared_ptr<Recording> &completed) {
            if (settings.gpuTiming && completed->context == context && context &&
                completed->layer == context->layer) {
                completed->ReadTiming(status);
                // Verbose only: a periodic average so a run can be measured from the log instead of
                // from the panel. Only reachable when timing itself is on (panel or ForceTiming).
            }
            if (completed->meter && (completed->modelMeasurement || settings.panelStatistics) &&
                completed->whiteEpoch == whiteEpoch && !reset) {
                status.whiteMeasurementError =
                    completed->meter->Consume(white, &status.gpuExposure);
                if (SUCCEEDED(status.whiteMeasurementError)) {
                    status.gpuExposureValid =
                        completed->hasExposure && std::isfinite(status.gpuExposure) &&
                        status.gpuExposure > 1e-6f && status.gpuExposure < 1e6f;
                    status.gpuDivisorValid = true;
                    float divisor = completed->fallbackWhite;
                    if (!completed->sceneLinear)
                        divisor = 1;
                    else if (completed->liveExposure && status.gpuExposureValid) {
                        // The reverse ratio flips the fraction; the texture value itself is never inverted.
                        const float ratio =
                            completed->liveExposure == 2
                                ? status.gpuExposure / completed->preMultiplier
                                : completed->preMultiplier / status.gpuExposure;
                        divisor = Clamped(std::pow((std::max)(ratio, 1e-8f), completed->whiteExponent) *
                                              completed->whiteTrim,
                                          1e-8f, 1024, 1);
                    }
                    status.gpuDivisor = divisor;
                }
            }
            if (completed->heap)
                reusableHeap = completed->heap;
            completed->RetireInputs();
        };
        // Completion protects descriptor writes/readback/release, not same-queue execution.
        for (auto it = inFlight.begin(); it != inFlight.end();) {
            if ((*it)->input.ticket->Complete()) {
                retire(*it);
                it = inFlight.erase(it);
            } else
                ++it;
        }
        if (previous) {
            if (previous->input.ticket->Complete())
                retire(previous);
            else
                inFlight.push_back(previous);
        }
        previous.reset();
        status.white = white.Resolve(settings.white, exposure, dt);
        if (!exposure.sceneLinear)
            status.white.value = status.white.target = 1; // SDR bypass actually normalizes by one
        if (!context || context->w != color.Width || context->h != color.Height ||
            context->format != format) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                      sizeof(support)));
            if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
                return refuse(E_NOTIMPL, "Color output format cannot be written by NR resolve");
            context = std::make_shared<Context>();
            context->w = unsigned(color.Width);
            context->h = color.Height;
            context->format = format;
            context->tuning = settings;
            // A style/strength change does not change the input exposure domain.
            // Preserve its measured white instead of alternating fallback and measurement.
            frameIndex = 0;
            status.modelError = S_OK;
            status.timingValid = false;
        }
        if (!SameTuning(context->tuning, settings)) {
            // Opt retains same-sized scratch textures when only model tuning changes.
            // Each Recording owns its feature independently until GPU retirement.
            context->layer.reset();
            context->tuning = settings;
            context->failed = false;
            frameIndex = 0;
            status.modelError = S_OK;
            status.timingValid = false;
        }
        auto ctx = context;
        if (ctx->failed)
            return status.error == S_OK ? E_FAIL : status.error;
        status.actualPasses = 0;
        status.readyPasses = status.failedPass = 0;
        if (ctx->layer && ctx->layer->feature && !ctx->layer->failed)
            status.readyPasses = 1;
        if (reset && ctx->layer) {
            // Verbose only, and only on the transition: a reset request that is already set is the
            // same pending reset, not a new one.
            ctx->layer->first = true;
        }
        auto recording = std::make_shared<Recording>();
        recording->context = ctx;
        recording->layer = ctx->layer;
        recording->input = frame;
        recording->exposure = frame.exposureTexture;
        recording->exposureMetadata = frame.exposureMetadata;
        recording->whiteEpoch = whiteEpoch;
        if (recording->exposure) {
            const auto &metadata = recording->exposureMetadata;
            if (!metadata.valid || !Plain(metadata.description) ||
                metadata.description.Width != 1 || metadata.description.Height != 1 ||
                (metadata.description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                !ShaderViewCompatible(metadata.description.Format))
                recording->exposure.Reset();
        }
        status.gameExposureTexture = recording->exposure != nullptr;
        recording->hasExposure = status.gameExposureTexture;
        keep = recording; // registered before the first model/GPU operation
        backend.SetUnsigned("DLSSNR.Enabled", 1u);
        backend.SetUnsigned("DLSSNR.Width", ctx->w);
        backend.SetUnsigned("DLSSNR.Height", ctx->h);
        backend.SetUnsigned("CreationNodeMask", 1u);
        backend.SetUnsigned("VisibilityNodeMask", 1u);
        if (!ctx->layer) {
            // Only creation reads these; the evaluation path below sets the map again, so the
            // steady-state frame does not repeat them.
            Tuning(backend, settings);
            Resource(backend, "DLSSNR.ControlMask", nullptr);
            // Validate resources before letting the model record initialization.
            Shaders();
            auto layer = std::make_shared<Layer>();
            layer->backend = &backend;
            layer->last = frame.ticket;
            ctx->layer = layer;
            recording->layer = layer;
            previous = recording;
            layer->first = true;
            // Verbose only: the reason for the rebuild and what the NGX call actually cost on the
            // render thread. This is the only place a feature is created.
            const auto rc = backend.Create(list, &layer->feature);
            status.ngxResult = rc;
            status.ready = false;
            status.reason = "NR layer created; waiting for a subsequent submitted recording";
            if (rc != 1 || !layer->feature) {
                layer->failed = true;
                status.modelError = E_FAIL;
                status.failedPass = 1;
                status.reason = "NR feature creation failed; original frame preserved";
                ctx->failed = true;
                status.error = E_FAIL;
                return E_FAIL;
            }
            recording->createdFeature = true;
            status.error = S_OK;
            // Opt's main snippet path records GPU hangs when creation and evaluation
            // share a list. Submit creation first; evaluate on a later recording.
            return S_OK;
        }
        recording->root = root;
        recording->pipeline = pipeline;
        if (!ctx->proxy) {
            ctx->proxy = Texture(device.Get(), ctx->w, ctx->h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            ctx->answer = Texture(device.Get(), ctx->w, ctx->h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            // The output format was validated for UAV typed stores when this context was created.
            if (!resolveTarget)
                ctx->output = Texture(device.Get(), ctx->w, ctx->h, format);
        }
        if (!resolveTarget)
            resolveTarget = ctx->output.Get();
        if (settings.gpuTiming) {
            try {
                recording->Timing(AcquireTiming());
            } catch (HRESULT) {
                recording->timing.reset();
                status.timingValid = false;
            }
        } // optional diagnostics cannot disable NR
        recording->heap = reusableHeap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 24,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (!recording->heap)
            ThrowIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&recording->heap)));
        auto cpu = [&](unsigned n) {
            auto h = recording->heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(n) * device->GetDescriptorHandleIncrementSize(hd.Type);
            return h;
        };
        auto resourceDescription = [&](ID3D12Resource *r) {
            for (unsigned n = 0; n < 3; ++n)
                if (frame.resources[n].Get() == r && frame.resourceMetadata[n].valid)
                    return frame.resourceMetadata[n].description;
            if (recording->exposure.Get() == r && recording->exposureMetadata.valid)
                return recording->exposureMetadata.description;
            return r->GetDesc();
        };
        auto srv = [&](unsigned n, ID3D12Resource *r, bool depth = false) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = r ? ViewFormat(resourceDescription(r).Format, depth)
                         : DXGI_FORMAT_R16G16B16A16_FLOAT;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            device->CreateShaderResourceView(r, &v, cpu(n));
        };
        auto uav = [&](unsigned n, ID3D12Resource *r) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
            v.Format = r ? resourceDescription(r).Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(r, nullptr, &v, cpu(n));
        };
        if (recording->timing)
            list->EndQuery(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        srv(0, result);
        srv(1, nullptr);
        srv(2, nullptr);
        srv(3, recording->exposure.Get());
        uav(4, ctx->proxy.Get());
        uav(5, nullptr);
        // Lite's captured/shared input is already the owned, unedited image.
        // Reuse it for resolve instead of Lite's second half-float keep copy.
        srv(6, ctx->proxy.Get());
        srv(8, result);
        srv(9, recording->exposure.Get());
        uav(10, resolveTarget);
        uav(11, nullptr);
        struct Constants {
            unsigned mode, w, h, display;
            float white, preMul;
            unsigned live;
            unsigned guideX, guideY, guideW, guideH;
            float gain;
            unsigned composed;
            float exponent, trim;
            unsigned reserved;
        } c{};
        c.w = ctx->w;
        c.h = ctx->h;
        c.display = exposure.sceneLinear ? 0u : 1u;
        c.white = status.white.value;
        c.preMul = 1;
        c.gain = settings.differenceGain;
        c.composed = settings.composed ? 1u : 0u;
        c.exponent = settings.white.exponent;
        c.trim = settings.white.trim;
        const bool live =
            recording->exposure && exposure.sceneLinear && settings.white.useTexture &&
            settings.white.mode == WhiteMode::Game;
        if (live) {
            // 2 selects the swapped fraction; the shader keeps the texture value as is.
            c.live = settings.white.reverseRatio ? 2u : 1u;
            // The shader applies the exponent and trim, so only the raw pre-exposure is sent.
            // A missing or non-positive Pre.Exposure substitutes 1, matching the scalar path.
            const bool preUsable = exposure.prePresent && exposure.pre > 0;
            c.preMul = settings.white.usePreExposure && preUsable
                            ? Clamped(exposure.pre, 1e-8f, 1e6f, 1)
                            : 1;
            status.white.source = WhiteSource::Game;
        }
        recording->sceneLinear = exposure.sceneLinear;
        recording->liveExposure = c.live;
        recording->preMultiplier = c.preMul;
        recording->whiteExponent = c.exponent;
        recording->whiteTrim = c.trim;
        recording->fallbackWhite = c.white;
        auto run = [&](unsigned offset) {
            auto *heap = recording->heap.Get();
            list->SetDescriptorHeaps(1, &heap);
            list->SetComputeRootSignature(root.Get());
            list->SetPipelineState(pipeline.Get());
            auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(offset) * device->GetDescriptorHandleIncrementSize(hd.Type);
            list->SetComputeRootDescriptorTable(0, gpu);
            list->SetComputeRoot32BitConstants(1, 16, &c, 0);
            list->Dispatch((ctx->w + 7) / 8, (ctx->h + 7) / 8, 1);
        };
        ID3D12Resource *guides[2]{frame.resources[1].Get(), frame.resources[2].Get()};
        for (unsigned n = 0; n < 2; ++n) {
            auto d = resourceDescription(guides[n]);
            const auto expected = n ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32_FLOAT;
            // The model builds its own views from the resource descriptor, so only a guide that is
            // already typed exactly this way can be handed over: a typeless member of the same
            // family has to be re-typed first, even though a shader view of it would be valid.
            if (d.Format == expected && !(n == 0 && frame.guides.flipDepth))
                continue;
            auto &twin = ctx->guides[n];
            if (!twin || twin->GetDesc().Width != d.Width || twin->GetDesc().Height != d.Height ||
                twin->GetDesc().Format != expected)
                twin = Texture(device.Get(), unsigned(d.Width), d.Height, expected);
            else
                Transition(list, twin.Get(), SRV, UAV);
            const unsigned table = 12 + 6 * n;
            srv(table, guides[n], n == 0);
            for (unsigned j = 1; j < 4; ++j)
                srv(table + j, nullptr);
            uav(table + 4, twin.Get());
            uav(table + 5, nullptr);
            auto *heap = recording->heap.Get();
            list->SetDescriptorHeaps(1, &heap);
            list->SetComputeRootSignature(root.Get());
            list->SetPipelineState(pipeline.Get());
            auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(table) * device->GetDescriptorHandleIncrementSize(hd.Type);
            list->SetComputeRootDescriptorTable(0, gpu);
            auto guideConstants = c;
            guideConstants.mode = 2;
            guideConstants.w = unsigned(d.Width);
            guideConstants.h = d.Height;
            guideConstants.display = n == 0 && frame.guides.flipDepth;
            guideConstants.guideX = frame.guides.originX[n + 1];
            guideConstants.guideY = frame.guides.originY[n + 1];
            guideConstants.guideW =
                frame.guides.width[n + 1] ? frame.guides.width[n + 1] : unsigned(d.Width);
            guideConstants.guideH =
                frame.guides.height[n + 1] ? frame.guides.height[n + 1] : d.Height;
            list->SetComputeRoot32BitConstants(1, 16, &guideConstants, 0);
            list->Dispatch((guideConstants.w + 7) / 8, (guideConstants.h + 7) / 8, 1);
            Transition(list, twin.Get(), UAV, SRV);
            guides[n] = twin.Get();
            recording->modelGuides[n] = twin;
        }
        // Measure unedited input even with a closed panel, only when NR needs measured white.
        // Game-mode white point runs entirely in the shader, so only Measured needs a readback.
        const bool modelMeasurement =
            exposure.sceneLinear &&
            settings.white.mode == WhiteMode::Measured;
        recording->modelMeasurement = modelMeasurement;

        const bool measure = recording->modelMeasurement || settings.panelStatistics;
        if (!measure)
            meter.reset(); // Recordings retain any submitted readback independently.
        if (measure && (frameIndex++ % std::clamp(settings.white.every, 1u, 120u) == 0)) {
            HRESULT hr{};
            try {
                if (!meter)
                    meter = std::make_unique<WhiteMeter12>(device.Get());
                hr = meter->Record(list, result, recording->meter, recording->exposure.Get(),
                                   frame.ticket);
            } catch (HRESULT error) {
                hr = error;
            }
            status.whiteMeasurementError = hr;
            if (FAILED(hr))
                status.reason = "White meter unavailable; manual/history fallback";
        }
        run(0);
        Transition(list, ctx->proxy.Get(), UAV, SRV);
        // One feature evaluates the immutable encoded input exactly once.
        bool readable{};
        bool evaluatedOk{};
        ID3D12Resource *input = ctx->proxy.Get();
        ID3D12Resource *final{};
        if (status.readyPasses) {
            auto &layer = ctx->layer;
            auto *answer = ctx->answer.Get();
            // Nothing is bound as DLSSNR.ControlMask: the model derives its own application mask.
            Resource(backend, "DLSSNR.Color", input);
            Resource(backend, "DLSSNR.Output", answer);
            Resource(backend, "DLSSNR.ControlMask", nullptr);
            Tuning(backend, settings);
            backend.SetUnsigned("DLSSNR.Reset", layer->first ? 1u : 0u);
            Resource(backend, "DLSSNR.Depth", guides[0]);
            Resource(backend, "DLSSNR.MVec", guides[1]);
            backend.SetUnsigned("DLSSNR.DepthInverted", frame.guides.depthInverted ? 1u : 0u);
            backend.SetFloat("DLSSNR.MVecScaleX", frame.guides.motionScaleX);
            backend.SetFloat("DLSSNR.MVecScaleY", frame.guides.motionScaleY);
            Region(backend, "Color", ctx->w, ctx->h);
            Region(backend, "Output", ctx->w, ctx->h);
            for (unsigned g = 1; g < 3; ++g) {
                const auto &d = frame.resourceMetadata[g].description;
                Region(backend, g == 1 ? "Depth" : "MVec",
                       frame.guides.width[g] ? frame.guides.width[g] : unsigned(d.Width),
                       frame.guides.height[g] ? frame.guides.height[g] : d.Height,
                       frame.guides.originX[g], frame.guides.originY[g]);
            }
            layer->last = frame.ticket;
            previous = recording; // retain even partially recorded model work
            if (recording->timing)
                list->EndQuery(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
            const auto rc = backend.Evaluate(list, layer->feature);
            status.ngxResult = rc;
            if (recording->timing) {
                list->EndQuery(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
                recording->timedLayers = 1;
            }
            if (rc != 1) {
                layer->failed = true;
                status.modelError = E_FAIL;
                status.failedPass = 1;
                status.readyPasses = 0;
            } else {
                layer->first = false;
                status.actualPasses = 1;
                recording->evaluated = true;
                evaluatedOk = true;
                Transition(list, answer, UAV, SRV);
                readable = true;
                final = answer;
            }
        }
        if (final) {
            srv(7, final);
            if (!frame.resolveTarget && ctx->outputReadable)
                Transition(list, ctx->output.Get(), SRV, UAV);
            c.mode = 1;
            run(6);
            if (!frame.resolveTarget) {
                // Private output is read by the color copy-back; the game UAV used by
                // DirectResolve already stays in its expected UAV state.
                Transition(list, resolveTarget, UAV, SRV);
                ctx->outputReadable = true;
            }
            result = resolveTarget;
            status.edited = status.ready = true;
            status.error = S_OK;
            status.reason = "NR evaluated successfully; resolve recorded";
        } else if (!evaluatedOk) {
            ctx->failed = true;
            status.ready = false;
            status.error = E_FAIL;
            status.reason = "First NR layer evaluation failed; original frame preserved";
        }
        if (readable)
            Transition(list, ctx->answer.Get(), SRV, UAV);
        Transition(list, ctx->proxy.Get(), SRV, UAV);
        if (recording->timing) {
            list->EndQuery(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                   0, 2 + 2 * recording->timedLayers,
                                   recording->timing->times.Get(), 0);
        }
        previous = recording;
        return S_OK; // partial GPU work must still be submitted on failure
    }
};
PassExecutor12::PassExecutor12(ID3D12Device *d, FeatureBackend &b)
    : impl(std::make_unique<Impl>(d, b)) {
}
PassExecutor12::~PassExecutor12() {
    impl.reset();
    DrainParkedFeatures();
}
HRESULT PassExecutor12::Record(ID3D12GraphicsCommandList *list, const PassFrame &f, const Exposure &e,
                               const PassSettings &s, bool reset,
                               std::shared_ptr<void> &keep, ID3D12Resource *&result) {
    std::lock_guard lock(impl->mutex);
    HRESULT hr = S_OK;
    try {
        hr = impl->Record(list, f, e, s, reset, keep, result);
        if (FAILED(hr)) {
            impl->status.error = hr;
            impl->status.ready = false;
        }
    } catch (HRESULT error) {
        hr = error;
        impl->status.error = hr;
        impl->status.ready = false;
        impl->status.reason = "NR resource operation failed; original frame preserved";
    } catch (...) {
        hr = E_FAIL;
        impl->status.error = E_FAIL;
        impl->status.ready = false;
    }
    return hr;
}
PassStatus PassExecutor12::Snapshot() const {
    std::lock_guard lock(impl->mutex);
    return impl->status;
}
void PassExecutor12::InvalidateHistory() {
    std::lock_guard lock(impl->mutex);
    if (impl->context)
        if (impl->context->layer) {
            impl->context->layer->first = true;
            // Verbose only: this discards temporal accumulation, so its frequency matters.
        }
}
void PassExecutor12::RetireCompletedInputs() {
    std::lock_guard lock(impl->mutex);
    if (impl->previous)
        impl->previous->RetireInputs();
    for (auto &recording : impl->inFlight)
        recording->RetireInputs();
}
}
