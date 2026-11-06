// Guide previews: Lite's shaders/dlssnr.hlsl view modes, separated from the model pass.
#include "preview.hpp"
#include "lite_preview_shader.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace litedx12::nr
{
Preview12::Preview12(ID3D12Device* d)
    : device(d)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    ComputePipeline(device.Get(), ranges, 2, 12, litePreviewShader, litePreviewShaderSize,
                    root.ReleaseAndGetAddressOf(), pipeline.ReleaseAndGetAddressOf());
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ThrowIfFailed(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap)));
}
HRESULT Preview12::Record(ID3D12GraphicsCommandList* list, const PassFrame& frame)
{
    if (!list)
    {
        return E_INVALIDARG;
    }
    if (!frame.resources[0])
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    if (!frame.resourceMetadata[0].valid)
    {
        return E_INVALIDARG;
    }
    auto d = frame.resourceMetadata[0].description;
    const unsigned w = frame.guides.width[0] ? frame.guides.width[0] : unsigned(d.Width), h = frame.guides.height[0] ? frame.guides.height[0] : d.Height;
    if (!w || !h || w > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION / 3)
    {
        return E_INVALIDARG;
    }
    if (!image || image->GetDesc().Width != w * 3 || image->GetDesc().Height != h)
    {
        d.Alignment = 0;
        d.Width = w * 3;
        d.Height = h;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc = {1, 0};
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&image));
        if (FAILED(hr))
        {
            return hr;
        }
    }
    Transition(list, image.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    HRESULT error = S_OK;
    for (unsigned n = 0; n < 3; ++n)
    {
        // Missing tiles are never displayed; keep available previews usable.
        // This does not synthesize a depth/MV resource for model processing.
        if (!frame.resources[n])
        {
            continue;
        }
        // Depth is shown at its own scale; colour and motion follow the panel's.
        const auto hr = RecordTile(list, frame, n, w, h, n != 2 ? 1.f : frame.guides.displayScale);
        if (FAILED(hr))
        {
            error = hr;
        }
    }
    Transition(list, image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return error;
}
// One tile: colour (0), depth (1) or motion vectors (2), written into its own atlas subrect.
HRESULT Preview12::RecordTile(ID3D12GraphicsCommandList* list, const PassFrame& frame, unsigned input,
                              unsigned outputWidth, unsigned outputHeight, float displayScale)
{
    if (!list || input > 2)
    {
        return E_INVALIDARG;
    }
    auto* source = frame.resources[input].Get();
    if (!source)
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    if (!frame.resourceMetadata[input].valid)
    {
        return E_INVALIDARG;
    }
    const auto desc = frame.resourceMetadata[input].description;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
    {
        return E_NOTIMPL;
    }
    const auto& g = frame.guides;
    if (!ShaderViewCompatible(desc.Format, input == 1))
    {
        return E_NOTIMPL;
    }
    const unsigned sw = g.width[input] ? g.width[input] : unsigned(desc.Width);
    const unsigned sh = g.height[input] ? g.height[input] : desc.Height;
    if (!sw || !sh || g.originX[input] > desc.Width || sw > desc.Width - g.originX[input] ||
        g.originY[input] > desc.Height || sh > desc.Height - g.originY[input] ||
        !std::isfinite(g.motionScaleX) || !std::isfinite(g.motionScaleY) ||
        !std::isfinite(displayScale) || displayScale <= 0)
    {
        return E_INVALIDARG;
    }
    const unsigned ow = outputWidth ? outputWidth : sw, oh = outputHeight ? outputHeight : sh;
    // Each tile owns one (SRV, UAV) pair in the atlas-wide heap.
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu.ptr += uint64_t(2 * input) * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = ViewFormat(desc.Format, input == 1);
    if (input == 2 && srv.Format != DXGI_FORMAT_R16G16_FLOAT && srv.Format != DXGI_FORMAT_R32G32_FLOAT &&
        srv.Format != DXGI_FORMAT_R16G16_SNORM)
    {
        return E_NOTIMPL;
    }
    if (input == 1 && srv.Format != DXGI_FORMAT_R32_FLOAT && srv.Format != DXGI_FORMAT_R16_FLOAT && srv.Format != DXGI_FORMAT_R16_UNORM &&
        srv.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS && srv.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
    {
        return E_NOTIMPL;
    }
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{srv.Format};
    auto hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(hr) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD))
    {
        return E_NOTIMPL;
    }
    device->CreateShaderResourceView(source, &srv, cpu);
    cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(image.Get(), nullptr, &uav, cpu);
    struct Constants
    {
        unsigned mode, ox, oy, sw, sh, ow, oh, flags;
        float sx, sy, gain;
        unsigned tileX;
    };
    const Constants c{input + 1u, g.originX[input], g.originY[input], sw, sh, ow, oh,
                      (g.depthInverted ? 1u : 0u) | (g.flipDepth ? 2u : 0u) | (g.depthContrast ? 4u : 0u), g.motionScaleX, g.motionScaleY, displayScale, ow * input};
    auto* h = heap.Get();
    list->SetDescriptorHeaps(1, &h);
    list->SetPipelineState(pipeline.Get());
    list->SetComputeRootSignature(root.Get());
    auto table = heap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += uint64_t(2 * input) * stride;
    list->SetComputeRootDescriptorTable(0, table);
    list->SetComputeRoot32BitConstants(1, 12, &c, 0);
    list->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    return S_OK;
}
}
