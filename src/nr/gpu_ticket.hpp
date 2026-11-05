// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <memory>
namespace litedx12::nr {
// A recording is not a submission. A failed signal is never treated as completion.
class GpuTicket {
public:
    enum class State {Pending,Submitted,Discarded};
    void Submit(ID3D12Fence* f,UINT64 v,ID3D12CommandQueue* queue=nullptr) {
        std::lock_guard lock(mutex);fence=f;value=v;submissionQueue=queue;state=State::Submitted;dependency.reset();
        if(timingRequested&&queue&&!frequency&&FAILED(queue->GetTimestampFrequency(&frequency)))frequency=0;
    }
    void RequestTiming() {std::lock_guard lock(mutex);timingRequested=true;}
    UINT64 TimestampFrequency() const {std::lock_guard lock(mutex);return frequency;}
    void Discard() {std::lock_guard lock(mutex);if(state==State::Pending){state=State::Discarded;dependency.reset();}}
    State Status() const {std::lock_guard lock(mutex);return state;}
    bool Complete() const {std::lock_guard lock(mutex);if(state!=State::Submitted||!fence||!value)return false;auto done=fence->GetCompletedValue();return done!=UINT64_MAX&&done>=value;}
    bool CanOrderSubmission() const {std::lock_guard lock(mutex);return state==State::Submitted&&fence&&value&&submissionQueue;}
    bool DependOn(const std::shared_ptr<GpuTicket>& previous) {
        if(!previous||previous.get()==this||!previous->CanOrderSubmission())return false;
        std::lock_guard lock(mutex);
        if(state!=State::Pending)return false;
        dependency=previous;return true;
    }
    // Only the submission owner calls this BEFORE Execute. Same-queue order
    // needs no wait; a different queue must wait on the real predecessor fence.
    HRESULT PrepareSubmission(ID3D12CommandQueue* queue) {
        std::lock_guard lock(mutex);
        if(!dependency)return S_OK;
        if(!queue)return E_POINTER;
        auto predecessor=dependency; // Keep its mutex alive through unlock.
        std::lock_guard previousLock(predecessor->mutex);
        if(dependency->state!=State::Submitted||!dependency->fence||!dependency->value||!dependency->submissionQueue)return E_ABORT;
        HRESULT hr=S_OK;
        const auto done=dependency->fence->GetCompletedValue();
        if(done==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        if(queue!=dependency->submissionQueue.Get()&&done<dependency->value)
            hr=queue->Wait(dependency->fence.Get(),dependency->value);
        // Keep the dependency until actual Submit/Discard. A later failure in
        // the batch must not erase ordering required by a retried submission.
        return hr;
    }
private:
    mutable std::mutex mutex;
    State state=State::Pending;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> submissionQueue;
    UINT64 value{};
    UINT64 frequency{};
    bool timingRequested{};
    std::shared_ptr<GpuTicket> dependency;
};
}
