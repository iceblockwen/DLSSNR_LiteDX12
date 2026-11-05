// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <memory>
namespace litedx12::nr
{
// Opt D3D12_Hooks / ScopedNrStateEnvelope: keep plugin bindings out of game tracking.
// NR off forwards calls; on requires creation/Reset/ClearState and, with
// Diagnostics.RequireComputeSignature, a captured compute root in the new epoch.
void SetCommandStateEnabled(bool) noexcept;
void InstallCommandState(ID3D12GraphicsCommandList*);
void InstallDeviceCommandState(ID3D12Device*);
void InstallD3D12CommandState(HMODULE);
class CommandStateScope
{
  public:
    explicit CommandStateScope(ID3D12GraphicsCommandList*);
    ~CommandStateScope();
    explicit operator bool() const
    {
        return saved != nullptr;
    }
    const char* Reason() const
    {
        return reason;
    }
    CommandStateScope(const CommandStateScope&) = delete;
    CommandStateScope& operator=(const CommandStateScope&) = delete;

  private:
    struct Saved;
    std::unique_ptr<Saved> saved;
    const char* reason = "DX12 binding snapshot unavailable";
};
}
