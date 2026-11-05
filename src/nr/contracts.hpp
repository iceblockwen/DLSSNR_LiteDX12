#pragma once
#include <d3d12.h>
namespace litedx12::nr
{
// Concrete frame and executor contracts: frame_input.hpp and pass.hpp.
using CreateFn = int(__cdecl*)(ID3D12GraphicsCommandList*, int, const void*, void**);
using EvaluateFn = int(__cdecl*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
using ReleaseFn = int(__cdecl*)(void*);
}
