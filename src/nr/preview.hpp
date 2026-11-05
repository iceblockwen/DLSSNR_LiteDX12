// SPDX-License-Identifier: GPL-3.0-or-later
// Guide previews (colour / depth / motion). Independent of NGX and the panel;
// descriptors and output belong to one GPU recording.
#pragma once
#include "gpu_util.hpp"
#include "frame_input.hpp"
#include <array>
#include <memory>
namespace litedx12::nr
{
class Preview12
{
  public:
    explicit Preview12(ID3D12Device*);
    // All three inputs side by side: three tiles copied into one image.
    HRESULT Record(ID3D12GraphicsCommandList*, const PassFrame&);
    ComPtr<ID3D12Resource> image;

  private:
    HRESULT RecordTile(ID3D12GraphicsCommandList*, const PassFrame&, unsigned input,
                       unsigned outputWidth, unsigned outputHeight, float displayScale);
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::array<std::unique_ptr<Preview12>, 3> tiles;
};
}
