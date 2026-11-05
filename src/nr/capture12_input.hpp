// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gpu_util.hpp"
#include "frame_input.hpp"
#include <array>
namespace litedx12::nr
{
inline void TransitionSubresource(ID3D12GraphicsCommandList* list, ID3D12Resource* r, UINT subresource,
                                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (!r || before == after)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, subresource, before, after};
    list->ResourceBarrier(1, &b);
}
// One ResourceBarrier call per group instead of one per resource: the copies of a
// frame stay between the two flushes, and nothing between them is reordered.
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
        b.Transition = {resource, 0, before, after};
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
inline bool DirectShaderInput12(const D3D12_RESOURCE_DESC& d, bool depth = false)
{
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
           d.MipLevels == 1 && d.SampleDesc.Count == 1 && !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
           ShaderViewCompatible(d.Format, depth);
}
// Plan aliases before recording anything; restore even on refusal/exception.
// Captured12/Recording own the resources, not this stack state guard.
class InputReadStates12
{
  public:
    explicit InputReadStates12(ID3D12GraphicsCommandList* input)
        : list(input)
    {
    }
    ~InputReadStates12()
    {
        Restore();
    }
    InputReadStates12(const InputReadStates12&) = delete;
    InputReadStates12& operator=(const InputReadStates12&) = delete;
    void Add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before)
    {
        for (unsigned n = 0; n < count; ++n)
        {
            if (entries[n].resource == resource)
            {
                if (entries[n].before != before)
                {
                    throw E_INVALIDARG;
                }
                return;
            }
        }
        if (count == entries.size())
        {
            throw E_INVALIDARG;
        }
        entries[count++] = {resource, before};
    }
    void Begin()
    {
        if (active)
        {
            return;
        }
        // Only subresource 0 is read by the model. Whole-resource barriers would also redeclare
        // the states of a game target's remaining subresources, which this frame never declares,
        // and force the driver to flush the entire surface.
        for (unsigned n = 0; n < count; ++n)
        {
            TransitionSubresource(list, entries[n].resource, 0, entries[n].before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        active = true;
    }
    void Restore() noexcept
    {
        if (!active)
        {
            return;
        }
        for (unsigned n = 0; n < count; ++n)
        {
            TransitionSubresource(list, entries[n].resource, 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, entries[n].before);
        }
        active = false;
    }

  private:
    struct Entry
    {
        ID3D12Resource* resource{};
        D3D12_RESOURCE_STATES before{};
    };
    ID3D12GraphicsCommandList* list;
    std::array<Entry, 4> entries{};
    unsigned count{};
    bool active{};
};
inline void CopySubresource0(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src)
{
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = dst;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = src;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}
inline void CopyShaderInput12(ID3D12GraphicsCommandList* list, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
                              ID3D12Resource* copy, D3D12_RESOURCE_STATES copyState)
{
    // Only the top mip is consumed, and a game target may carry a full mip chain
    // (the other subresources can be in states this frame never declared), so the
    // recording touches subresource 0 alone instead of the whole resource.
    BarrierBatch12 barriers(list);
    barriers.Add(copy, copyState, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.Add(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barriers.Flush();
    CopySubresource0(list, copy, source);
    barriers.Add(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    barriers.Add(copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barriers.Flush();
}
// Caller first restores any direct input read states. Resolve has finished
// reading the original color before the result overwrites it on the same list.
inline void CopyNrResult12(ID3D12GraphicsCommandList* list, ID3D12Resource* result, ID3D12Resource* color, D3D12_RESOURCE_STATES colorState)
{
    BarrierBatch12 barriers(list);
    barriers.Add(result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barriers.Add(color, colorState, D3D12_RESOURCE_STATE_COPY_DEST);
    barriers.Flush();
    CopySubresource0(list, color, result);
    barriers.Add(color, D3D12_RESOURCE_STATE_COPY_DEST, colorState);
    barriers.Add(result, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barriers.Flush();
}
}
