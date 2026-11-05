// SPDX-License-Identifier: GPL-3.0-or-later
#include "status.hpp"
#include "../graphics.hpp"
#include "../config_writer.hpp"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <set>
namespace litedx12::nr {
namespace {
struct StatusRegistry {
    std::recursive_mutex mutex;
    PreviewStatus status;
    std::set<const void*> visiblePanels;
};
StatusRegistry& Global() {
    static auto* r = new StatusRegistry;
    return *r;
} // pinned DLL; the block outlives the process' threads
std::atomic<bool> previewSelected{};
std::atomic<bool> panelVisible{};
std::atomic<bool> depthContrast{false};
std::atomic<bool> depthFlip{false};
std::atomic<float> gain{1};
void Save(const wchar_t* key, const wchar_t* value, const wchar_t* section = L"Diagnostics") {
    QueueConfig(section, key, value);
}
}
namespace detail {
std::recursive_mutex& StatusMutex() { return Global().mutex; }
PreviewStatus& StatusBlock() { return Global().status; }
}
// The status block is published under its own lock and never calls into the ledger.
bool PreviewSelected() {
    return previewSelected.load();
}
bool PanelVisible() {
    return panelVisible.load();
}
bool PreviewActive() {
    return PanelVisible() && PreviewSelected();
}
void SetPanelVisible(const void* owner, bool visible) {
    if (!owner)
        return;
    std::lock_guard lock(Global().mutex);
    auto& r = Global();
    if (visible)
        r.visiblePanels.insert(owner);
    else
        r.visiblePanels.erase(owner);
    panelVisible = !r.visiblePanels.empty();
}
bool DepthContrast() {
    return depthContrast.load();
}
void SetDepthContrast(bool value, bool persist) {
    depthContrast = value;
    if (persist)
        Save(L"DepthContrast", value ? L"1" : L"0");
}
bool DepthFlip() {
    return depthFlip.load();
}
void SetDepthFlip(bool value, bool persist) {
    depthFlip = value;
    if (persist)
        Save(L"DepthUpsideDown", value ? L"1" : L"0", L"Resources");
}
void SetPreviewSelected(bool value) {
    previewSelected = value;
}
float DisplayScale() {
    return gain.load();
}
void SetDisplayScale(float value, bool persist) {
    if (!std::isfinite(value))
        return;
    value = std::clamp(value, .001f, 64.f);
    gain = value;
    if (persist) {
        wchar_t text[32]{};
        swprintf_s(text, L"%.6g", double(value));
        Save(L"ResourceScale", text);
    }
}
PreviewStatus PreviewSnapshot() {
    std::lock_guard lock(Global().mutex);
    return Global().status;
}
void PreviewResult(HRESULT hr) {
    std::lock_guard lock(Global().mutex);
    Global().status.previewError = hr;
}
}
