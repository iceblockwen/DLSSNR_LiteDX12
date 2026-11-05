// SPDX-License-Identifier: GPL-3.0-or-later
// Shared Direct3D 12 helpers: COM pointer alias, throw-on-failure, the DXGI format
// mapping the model's views need, and a resource barrier helper. No plugin state.
#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
namespace litedx12::nr {
using Microsoft::WRL::ComPtr;
inline void ThrowIfFailed(HRESULT hr) { if (FAILED(hr)) throw hr; }
inline float Clamped(float v, float lo, float hi, float fallback) {
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
}
