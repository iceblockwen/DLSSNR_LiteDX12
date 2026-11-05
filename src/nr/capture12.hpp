// SPDX-License-Identifier: GPL-3.0-or-later
// The DX12 frame pipeline. It observes the game's own NGX SR evaluate, captures
// that frame's colour/depth/motion, runs the NR pass on the game's own command
// list, and retires every resource on the real fence. This is the "frame ledger":
// pending recordings, submitted recordings, the copy pool used when a guide cannot
// be read in place, and the preview generation the panel displays.
#pragma once
#include "frame_input.hpp"
#include "status.hpp"
namespace litedx12::nr
{
// State recorded for one observed frame: the resources the recording may still
// reference, the fence that proves it finished, and the model's own keep-alive.
struct Captured12
{
    ComPtr<ID3D12Device> device;
    // The list this recording belongs to. Holding it keeps the address meaningful
    // for the pointer comparison in PrepareSubmission/Submitted.
    ComPtr<ID3D12GraphicsCommandList> list;
    PassFrame frame;
    std::array<ComPtr<ID3D12Resource>, 3> borrowed;
    ComPtr<ID3D12Fence> fence;
    UINT64 completion{};
    ComPtr<ID3D12Resource> borrowedExposure;
    std::shared_ptr<void> modelRecording;
    std::array<ComPtr<ID3D12Resource>, 3> copies;
    std::array<D3D12_RESOURCE_DESC, 3> copyDescs{};
    std::array<D3D12_RESOURCE_STATES, 3> copyStates{}, recordedCopyStates{};
    ComPtr<ID3D12Resource> exposureCopy;
    D3D12_RESOURCE_DESC exposureDesc{};
    D3D12_RESOURCE_STATES exposureCopyState{}, recordedExposureState{};
    // Game resources this recording borrowed. Dropping them is safe only once the
    // owning fence completed, so the model recording stays separate.
    void ClearBorrowed()
    {
        borrowed = {};
        borrowedExposure.Reset();
    }
};
struct GameDlssStatus
{
    bool observed{}, released{};
    unsigned result{};
    UINT64 lastEvaluation{};
};
GameDlssStatus GameDlssSnapshot();
// Called by the NGX hook with the resources the game handed its own upscaler.
HRESULT Capture12(ID3D12GraphicsCommandList*, const FrameInput&, void* modelStream = nullptr);
void ReleaseModelStream(void*);
void PrepareModel12(ID3D12Device*);
// Submission bookkeeping, driven by the queue/list hooks. A recording is only
// retired once its fence has really advanced.
void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
HRESULT PrepareSubmission(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
void Discard(ID3D12GraphicsCommandList*) noexcept;
bool Pending();
void RetireCompleted();
void ReleaseDevice12(ID3D12Device*);
// The preview image the last capture recorded, on the game's own list. The ledger owns it and
// recreates it when the extents change; the panel only displays it.
ComPtr<ID3D12Resource> PreviewImage12(ID3D12Device*);
FARPROC ResolveNgx(HMODULE, LPCSTR, FARPROC);
}
namespace litedx12
{
void InstallFrameQueueCapture(ID3D12CommandQueue*);
}
