// SPDX-License-Identifier: GPL-3.0-or-later
// Panel::Impl: the panel's state. Its methods are split across three files —
// panel.cpp (construction, window subclass and teardown), panel_gpu.cpp (the
// Direct3D 12 renderer) and panel_ui.cpp (the ImGui window). Nothing outside
// panel*.cpp sees this type.
#pragma once
#include "panel.hpp"
#include "graphics.hpp"
#include "window_subclass.hpp"
#include "menu_hotkey.hpp"
#include "nr/capture12.hpp"
#include "nr/model_runtime.hpp"
#include "nr/model_settings.hpp"
#include "nr/status.hpp"
#include <imgui.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <vector>
namespace litedx12 {
using Microsoft::WRL::ComPtr;
// Shared by the shell and the renderer; holds no state of its own.
namespace panel_detail {
inline void Check(HRESULT hr) {
    if (FAILED(hr))
        throw hr;
}
// The render-target view format for a swapchain buffer: an sRGB buffer is drawn
// through its UNORM sibling (the interface's colours are already sRGB).
inline DXGI_FORMAT RtvFormat(DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    return format;
}
}
struct Panel::Impl {
    IDXGISwapChain *swap{}; // Borrowed wrapper; the wrapper owns the native swapchain.
    // Cached in InitRenderer: a QueryInterface per frame bought nothing. Held only while the
    // renderer lives, because a retained interface would outlive the swapchain it belongs to.
    ComPtr<IDXGISwapChain3> swap3;
    HWND hwnd{};
    bool windowAttached{};
    ImGuiContext *context{};
    GraphicsStatus status;
    bool win32Ready{}, rendererReady{}, inputFocused{}, ignoreMenuRelease{};
    MenuHotkey menuHotkey;
    bool previewVisibility{}, scaleEdited{};
    std::array<float, 4> tuningValues{};
    std::array<bool, 4> tuningPending{};
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> rtvHeap, srvHeap;
    UINT srvStride{}; // constant per device; queried once in InitRenderer
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 nextFence{};
    size_t drawSerial{};
    float dpiScale = 1;
    float fontDpiScale = 1;
    ImGuiStyle baseStyle;
    ImVec2 layoutDisplay{};
    float layoutScale{};
    bool repairPosition = true;
    DXGI_FORMAT format{};
    ImTextureID previewTexture{};
    bool previewSelected{};
    UINT previewWidth{}, previewHeight{};
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ID3D12Resource *buffer{}; // Borrowed from the swapchain; no backbuffer ownership cycle.
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        UINT64 completion{};
        // Preview image whose extents and SRV descriptor are already written for this slot.
        ComPtr<ID3D12Resource> previewSized;
        // Our own reference to the ledger's preview image: it may be recreated when the extents
        // change, and the descriptor above must not point at a released resource.
        ComPtr<ID3D12Resource> previewSource;
    };
    std::vector<Frame> frames;
    // Where a failure happened; printed by Failure().
    const wchar_t *renderStage = L"panel input/window callback";
    // Committed on edit end, never per frame of a drag.
    bool CommitTuning(nr::ModelSettings *target = nullptr);
    void SyncPreviewVisibility();
    void MenuKey(bool down);
    void PollMenuKey();
    ~Impl();
    static WindowSubclassRegistry<Impl> &Windows();
    static bool RouteKeyboard(MSG &message, bool removed);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void Unsubclass();
    void Wait(UINT64 value);
    void Drain();
    void DestroyRenderer();
    void InitRenderer();
    void BuildResourceUi(float scale);
    void BuildPipelineStatus(const nr::ModelSettings &settings, const nr::PassStatus &pass,
                             const nr::ModelStatus &model);
    void BuildUi();
    void Draw12();
    void Failure(HRESULT hr);
};
}
