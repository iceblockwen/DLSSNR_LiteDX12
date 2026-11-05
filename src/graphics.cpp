// SPDX-License-Identifier: GPL-3.0-or-later
// The swapchain side of the panel, split out of panel.cpp: it owns the registry and
// creates and retires one panel per swapchain. No NR and no UI work happens here.
#include "graphics_registry.hpp"
#include <cstdio>
namespace litedx12 {
using Microsoft::WRL::ComPtr;
// Process-lifetime registry avoids GPU work during DLL destruction under loader lock.
GraphicsRegistry &Graphics() {
    static auto *registry = new GraphicsRegistry;
    return *registry;
}
std::recursive_mutex &GraphicsMutex() {
    return Graphics().mutex;
}
void TrackSwapchain(IDXGISwapChain *swap, IUnknown *device) noexcept {
    try {
        std::lock_guard lock(Graphics().mutex);
        if (Graphics().panels.contains(swap))
            return;
        ComPtr<ID3D12CommandQueue> queue;
        if (device)
            device->QueryInterface(IID_PPV_ARGS(&queue));
        auto panel = std::make_shared<Panel>(swap, queue.Get());
        Graphics().panels.emplace(swap, std::move(panel));
    } catch (HRESULT hr) {
        GraphicsFailure(hr, L"Unsupported swapchain, window or command queue");
    } catch (...) {
        GraphicsFailure(E_FAIL, L"Cannot attach DLSSNR_LiteDX12 panel");
    }
}
void DrawSwapchain(IDXGISwapChain *swap, UINT flags) noexcept {
    try {
        std::lock_guard lock(Graphics().mutex);
        const auto it = Graphics().panels.find(swap);
        if (it != Graphics().panels.end())
            it->second->Draw(flags);
    } catch (...) {
    }
}
HRESULT ResizeSwapchain(IDXGISwapChain *swap, bool before, HRESULT result,
                        ID3D12CommandQueue *queue) noexcept {
    try {
        std::lock_guard lock(Graphics().mutex);
        const auto it = Graphics().panels.find(swap);
        if (it == Graphics().panels.end())
            return S_OK;
        if (before)
            it->second->BeforeResize();
        else
            it->second->AfterResize(result, queue);
        return S_OK;
    } catch (HRESULT hr) {
        FailSwapchain(swap, hr);
        return hr;
    } catch (...) {
        FailSwapchain(swap, E_FAIL);
        return E_FAIL;
    }
}
void FailSwapchain(IDXGISwapChain *swap, HRESULT error) noexcept {
    try {
        std::lock_guard lock(Graphics().mutex);
        const auto it = Graphics().panels.find(swap);
        if (it != Graphics().panels.end())
            it->second->Fail(error);
    } catch (...) {
    }
}
void ReleaseSwapchain(IUnknown *swap) noexcept {
    try {
        std::lock_guard lock(Graphics().mutex);
        const auto it = Graphics().panels.find(reinterpret_cast<IDXGISwapChain *>(swap));
        if (it != Graphics().panels.end()) {
            auto panel = std::move(it->second);
            Graphics().panels.erase(it);
            panel->Close();
        }
    } catch (...) {
    }
}
void GraphicsFailure(HRESULT, const wchar_t *) noexcept {}
}
