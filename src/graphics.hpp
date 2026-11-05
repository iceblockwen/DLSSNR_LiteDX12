#pragma once
#include "runtime.hpp"
#include <dxgi1_6.h>
#include <d3d12.h>
#include <mutex>

namespace litedx12
{
struct GraphicsStatus
{
    State graphics = State::Waiting;
    State panel = State::Waiting;
    bool visible = false;
    unsigned width = 0, height = 0;
    HRESULT error = S_OK;
    wchar_t reason[256]{};
};
void AttachFactory(REFIID iid, void** output) noexcept;
void Log(const wchar_t* message) noexcept;
// graphics.cpp owns the panel registry; wrappers.hpp binds each swapchain through these.
std::recursive_mutex& GraphicsMutex();
void TrackSwapchain(IDXGISwapChain* swap, IUnknown* device) noexcept;
void DrawSwapchain(IDXGISwapChain* swap, UINT flags) noexcept;
HRESULT ResizeSwapchain(IDXGISwapChain* swap, bool before, HRESULT result,
                        ID3D12CommandQueue* queue = nullptr) noexcept;
void ReleaseSwapchain(IUnknown* swap) noexcept;
void FailSwapchain(IDXGISwapChain* swap, HRESULT error) noexcept;
}
