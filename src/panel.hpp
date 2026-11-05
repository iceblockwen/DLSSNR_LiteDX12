#pragma once
#include "graphics.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

namespace litedx12 {
class Panel {
public:
    Panel(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue);
    ~Panel();
    void Draw(UINT presentFlags) noexcept;
    void BeforeResize();
    void AfterResize(HRESULT result, ID3D12CommandQueue* replacementQueue = nullptr);
    void Close();
    void Fail(HRESULT error) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
