// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's Direct3D 12 renderer: one recorded list per backend render call, the
// ImGui backend, the descriptor heaps, and the fence a reused list must wait on.
#include "panel_impl.hpp"
#include "input.hpp"
#include "graphics_registry.hpp"
#include "nr/capture12.hpp"
#include "nr/status.hpp"
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <windows.h>
namespace litedx12 {
using panel_detail::Check;
using panel_detail::RtvFormat;
void Panel::Impl::Wait(UINT64 value) {
    if (!value || fence->GetCompletedValue() >= value)
        return;
    Check(fence->SetEventOnCompletion(value, event));
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        throw HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    Check(device12->GetDeviceRemovedReason());
}

void Panel::Impl::Drain() {
    if (queue && fence && nextFence)
        Wait(nextFence);
}

void Panel::Impl::DestroyRenderer() {
    if (context)
        ImGui::SetCurrentContext(context);
    if (rendererReady) {
        ImGui_ImplDX12_Shutdown();
        rendererReady = false;
    }
    frames.clear();
    rtvHeap.Reset();
    srvHeap.Reset();
    swap3.Reset();
    drawSerial = 0;
}

void Panel::Impl::InitRenderer() {
    DXGI_SWAP_CHAIN_DESC desc{};
    Check(swap->GetDesc(&desc));
    status.width = desc.BufferDesc.Width;
    status.height = desc.BufferDesc.Height;
    format = RtvFormat(desc.BufferDesc.Format);
    {
        Check(swap->QueryInterface(IID_PPV_ARGS(&swap3)));
        if (desc.SampleDesc.Count != 1 || desc.BufferCount < 2)
            throw E_NOTIMPL;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.NumDescriptors = desc.BufferCount;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Check(device12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap)));
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 1 + desc.BufferCount;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srvHeap)));
        frames.resize(desc.BufferCount);
        srvStride = device12->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto stride =
            device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (auto &frame : frames) {
            const auto index = static_cast<UINT>(&frame - frames.data());
            ComPtr<ID3D12Resource> buffer;
            Check(swap->GetBuffer(index, IID_PPV_ARGS(&buffer)));
            frame.buffer = buffer.Get();
            auto texture = frame.buffer->GetDesc();
            status.width = static_cast<UINT>(texture.Width);
            status.height = texture.Height;
            frame.rtv = handle;
            handle.ptr += stride;
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device12->CreateRenderTargetView(frame.buffer, &view, frame.rtv);
            Check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&frame.allocator)));
            Check(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              frame.allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&frame.list)));
            Check(frame.list->Close());
        }
        if (!ImGui_ImplDX12_Init(device12.Get(), static_cast<int>(frames.size()), format,
                                 srvHeap.Get(), srvHeap->GetCPUDescriptorHandleForHeapStart(),
                                 srvHeap->GetGPUDescriptorHandleForHeapStart()))
            throw E_FAIL;
        rendererReady = true;
        if (!ImGui_ImplDX12_CreateDeviceObjects())
            throw E_FAIL;
    }
    status.panel = State::Ready;
    status.error = S_OK;
    status.reason[0] = 0;
}

void Panel::Impl::Draw12() {
    // Match the ImGui backend's own render-call ring, rather than the swapchain index.
    auto &frame = frames[drawSerial % frames.size()];
    if (fence->GetCompletedValue() < frame.completion)
        return;
    if (!swap3)
        return;
    const auto index = swap3->GetCurrentBackBufferIndex();
    auto &target = frames.at(index);
    Check(frame.allocator->Reset());
    Check(frame.list->Reset(frame.allocator.Get(), nullptr));
    previewSelected = nr::PreviewSelected();
    previewTexture = 0;
    if (previewSelected) {
        // The capture recorded this on the game's list; here it is only displayed. Keep our own
        // reference so a later recreation cannot leave the descriptor pointing at freed memory.
        auto image = nr::PreviewImage12(device12.Get());
        if (image) {
            if (frame.previewSource.Get() != image.Get())
                frame.previewSource = std::move(image);
            previewWidth = unsigned(frame.previewSource->GetDesc().Width);
            previewHeight = frame.previewSource->GetDesc().Height;
            const auto slot = static_cast<UINT>(&frame - frames.data()) + 1;
            auto gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += slot * srvStride;
            previewTexture = reinterpret_cast<ImTextureID>(gpu.ptr);
            // The descriptor only changes with the image, so write it once per image.
            if (frame.previewSource.Get() != frame.previewSized.Get()) {
                auto cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += slot * srvStride;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MipLevels = 1;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                device12->CreateShaderResourceView(frame.previewSource.Get(), &srv, cpu);
                frame.previewSized = frame.previewSource;
            }
            nr::Transition(frame.list.Get(), frame.previewSource.Get(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    UpdatePanelPointer(hwnd);
    ImGui::NewFrame();
    BuildUi();
    ImGui::Render();
    if (ImGui::GetDrawData()->DisplaySize.x <= 0 || ImGui::GetDrawData()->DisplaySize.y <= 0) {
        frame.list->Close();
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    frame.list->ResourceBarrier(1, &barrier);
    frame.list->OMSetRenderTargets(1, &target.rtv, FALSE, nullptr);
    auto *heap = srvHeap.Get();
    frame.list->SetDescriptorHeaps(1, &heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), frame.list.Get());
    if (previewTexture)
        nr::Transition(frame.list.Get(), frame.previewSource.Get(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    frame.list->ResourceBarrier(1, &barrier);
    Check(frame.list->Close());
    ID3D12CommandList *list = frame.list.Get();
    queue->ExecuteCommandLists(1, &list);
    const auto completion = ++nextFence;
    Check(queue->Signal(fence.Get(), completion));
    frame.completion = completion;
    ++drawSerial;
}

}
