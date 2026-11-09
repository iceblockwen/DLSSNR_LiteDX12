// Shared Direct3D 12 helpers: COM pointer alias, throw-on-failure, the DXGI format
// mapping the model's views need, and a resource barrier helper. No plugin state.
#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
namespace litedx12::nr
{
using Microsoft::WRL::ComPtr;
inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw hr;
    }
}
inline float Clamped(float v, float lo, float hi, float fallback)
{
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
// The typed view a typeless/special format must be seen through. `depth` picks the
// R16 sibling (UNORM) over the R16 FLOAT one.
DXGI_FORMAT ViewFormat(DXGI_FORMAT format, bool depth = false);
// Copy/share-compatible storage format: depth and sRGB variants become typeless.
DXGI_FORMAT StorageFormat(DXGI_FORMAT format);
// Whether the format can be read through a shader view at all (typeless needs ViewFormat).
bool ShaderViewCompatible(DXGI_FORMAT format, bool depth = false);
void Transition(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES,
                D3D12_RESOURCE_STATES);
// Collects transitions so one ResourceBarrier call carries the whole group; the
// caller decides where the group has to become visible and flushes there.
class BarrierBatch12
{
  public:
    explicit BarrierBatch12(ID3D12GraphicsCommandList* input)
        : list(input)
    {
    }
    BarrierBatch12(const BarrierBatch12&) = delete;
    BarrierBatch12& operator=(const BarrierBatch12&) = delete;
    void Add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        AddSubresource(resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after);
    }
    void AddSubresource(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after)
    {
        if (!resource || before == after)
        {
            return;
        }
        if (count == entries.size())
        {
            Flush();
        }
        auto& b = entries[count];
        b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, subresource, before, after};
        ++count;
    }
    void Flush()
    {
        if (!count)
        {
            return;
        }
        list->ResourceBarrier(count, entries.data());
        count = 0;
    }

  private:
    ID3D12GraphicsCommandList* list;
    std::array<D3D12_RESOURCE_BARRIER, 12> entries{};
    unsigned count{};
};
// The layout every Lite compute shader uses: one descriptor table over `ranges`
// and, when `constantWords` is not zero, one 32-bit constant block at b0.
// A null *root is created here; an existing one is shared by the new pipeline.
void ComputePipeline(ID3D12Device*, const D3D12_DESCRIPTOR_RANGE* ranges, unsigned rangeCount,
                     unsigned constantWords, const void* bytecode, size_t bytecodeSize,
                     ID3D12RootSignature** root, ID3D12PipelineState** pipeline);
}
