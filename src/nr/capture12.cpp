#include "capture12.hpp"
#include "preview.hpp"
#include "../graphics.hpp"
#include "../runtime.hpp"
#include "model_settings.hpp"
#include "model_runtime.hpp"
#include "capture12_input.hpp"
#include "command_guard.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
namespace litedx12::nr
{
namespace
{
struct FrameLedger
{
    std::recursive_mutex mutex;
    std::map<IUnknown*, std::shared_ptr<Captured12>> pending;
    std::vector<std::shared_ptr<Captured12>> submitted;
    std::map<ID3D12Device*, std::vector<std::shared_ptr<Captured12>>> copyPool;
    // Preview pipelines and their image, per device. Recorded on the game's list so the preview
    // never depends on an input outliving the frame.
    std::map<ID3D12Device*, std::shared_ptr<Preview12>> previews;
    std::map<std::pair<ID3D12Device*, void*>, std::unique_ptr<PassExecutor12>> model12;
    // Executors whose game stream went away, kept so a replacement stream reuses the NR feature and
    // its full-frame textures instead of paying one NGX CreateFeature plus ~165 MB at 4K.
    std::vector<std::unique_ptr<PassExecutor12>> spare12;
    UINT64 modelRevision{};
};
FrameLedger& Ledger()
{
    static auto* l = new FrameLedger;
    return *l;
} // pinned DLL; no GPU work under loader lock
bool SameCopy(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
           a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality && a.Flags == b.Flags &&
           a.Layout == b.Layout && a.Alignment == b.Alignment;
}
std::shared_ptr<Captured12> AcquireCopy(ID3D12Device* device)
{
    auto& pool = Ledger().copyPool[device];
    // A completed fence is necessary but insufficient: a submission may still own it.
    for (auto& slot : pool)
    {
        if (slot.use_count() == 1)
        {
            return slot;
        }
    }
    if (pool.size() >= 8)
    {
        return {};
    }
    auto slot = std::make_shared<Captured12>();
    slot->device = device;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot->fence)));
    pool.push_back(slot);
    return slot;
}
void EnsureCopy(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_DESC& cached,
                D3D12_RESOURCE_STATES& state)
{
    if (resource && SameCopy(desc, cached))
    {
        return;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> replacement;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&replacement)));
    resource = std::move(replacement);
    cached = desc;
    state = D3D12_RESOURCE_STATE_COPY_DEST;
}
std::atomic<size_t> pendingCount{};
std::atomic<size_t> submittedCount{};
std::atomic<bool> modelCached{};
// The native object owns the cookie, not the other way around: final destruction
// cancels an unsubmitted recording, and the raw COM address cannot be reused
// while the cookie lives.
constexpr GUID RecordingCookieId{
    0x9f7a6ce1, 0x6a23, 0x46bc, {0xab, 0x61, 0x32, 0x8f, 0x50, 0x88, 0x25, 0x91}};
// COM guarantees stable IUnknown identity, not stable addresses for other
// interfaces. NGX/queue wrappers can return a different graphics-list interface.
ComPtr<IUnknown> ListIdentity(IUnknown* list)
{
    ComPtr<IUnknown> identity;
    if (list)
    {
        ThrowIfFailed(list->QueryInterface(IID_PPV_ARGS(&identity)));
    }
    return identity;
}
void DiscardIdentity(IUnknown*) noexcept;
// The recording's own list pointer answers the common case without a
// QueryInterface; a different interface for the same object falls back to COM identity.
using PendingMap = std::map<IUnknown*, std::shared_ptr<Captured12>>;
PendingMap::iterator PendingJob(PendingMap& pending, ID3D12CommandList* list)
{
    for (auto it = pending.begin(); it != pending.end(); ++it)
    {
        if (it->second->list.Get() == list)
        {
            return it;
        }
    }
    const auto identity = ListIdentity(list);
    if (!identity)
    {
        return pending.end();
    }
    return pending.find(identity.Get());
}
class RecordingCookie final : public IUnknown
{
    std::atomic<ULONG> references{1};
    IUnknown* identity;

  public:
    explicit RecordingCookie(IUnknown* p)
        : identity(p)
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out)
        {
            return E_POINTER;
        }
        *out = nullptr;
        if (iid != IID_IUnknown)
        {
            return E_NOINTERFACE;
        }
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto count = --references;
        if (!count)
        {
            // The native object is being destroyed; never QueryInterface it here.
            DiscardIdentity(identity);
            delete this;
        }
        return count;
    }
};
void PublishCaptureBusy()
{
    auto status = LastPass();
    status.error = S_FALSE;
    status.ready = status.edited = false;
    status.frameSkipped = true;
    status.actualPasses = status.failedPass = 0;
    status.reason = "NR capture slots busy; original game frame preserved";
    PublishPass(status);
}
// Why this frame has no NR. frameSkipped keeps it informational: a rejected
// input must not latch the model off, it is retried on the next evaluation.
void PublishCaptureFailure(HRESULT hr, const char* reason)
{
    PassStatus status;
    status.error = hr;
    status.ready = status.edited = false;
    status.frameSkipped = true;
    status.actualPasses = status.failedPass = 0;
    status.reason = reason;
    PublishPass(status);
}
void Collect()
{
    bool retired{};
    auto& l = Ledger();
    for (auto it = l.submitted.begin(); it != l.submitted.end();)
    {
        auto job = *it;
        const auto done = job->fence ? job->fence->GetCompletedValue() : 0;
        if (done != UINT64_MAX && job->completion && done >= job->completion)
        {
            job->ReleaseFrame(); // safe only at GPU completion
            retired = true;
            it = l.submitted.erase(it);
        }
        else
        {
            ++it;
        }
    }
    submittedCount = l.submitted.size();
    if (retired)
    {
        for (auto& [key, pass] : l.model12)
        {
            (void)key;
            pass->RetireCompletedInputs();
        }
    }
    RetireModelFeatures();
}
} // namespace
bool Pending()
{
    return pendingCount.load() != 0;
}
void RetireCompleted()
{
    if (!submittedCount.load() && (!modelCached.load() || ModelRequested()) &&
        !HasParkedModelFeatures())
    {
        return;
    }
    std::lock_guard lock(Ledger().mutex);
    Collect();
    // Destroying an executor drains the features it parked.
    if (!ModelRequested() && modelCached.exchange(false))
    {
        Ledger().model12.clear();
        Ledger().spare12.clear();
    }
}
void PrepareModel12(ID3D12Device* device)
{
    if (!device || !ModelActive())
    {
        return;
    }
    // Render thread only. Initialize() short-circuits a bound device under the
    // runtime lock; the panel calls this every frame, so skip the lock entirely.
    // Holding the reference keeps the address from being reused by a new device.
    static ComPtr<ID3D12Device> prepared;
    if (prepared.Get() == device)
    {
        return;
    }
    if (SUCCEEDED(Runtime().Initialize(device)))
    {
        prepared = device;
    }
}
void ReleaseDevice12(ID3D12Device* device)
{
    std::lock_guard lock(Ledger().mutex);
    auto& l = Ledger();
    l.previews.erase(device);
    l.copyPool.erase(device);
    Collect();
    for (auto it = l.model12.begin(); it != l.model12.end();)
    {
        if (it->first.first == device)
        {
            it = l.model12.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // A parked executor owns GPU objects of the device being retired.
    l.spare12.clear();
}
void ReleaseModelStream(void* stream)
{
    std::lock_guard lock(Ledger().mutex);
    auto& l = Ledger();
    for (auto it = l.model12.begin(); it != l.model12.end();)
    {
        if (it->first.second != stream)
        {
            ++it;
            continue;
        }
        // The game released its own SR feature; ours is independent of that lifetime. Park the
        // executor and only drop its temporal history, so the next stream reuses the feature.
        if (l.spare12.size() < 2)
        {
            it->second->InvalidateHistory(L"the game released its feature; the executor is parked");
            l.spare12.push_back(std::move(it->second));
        }
        it = l.model12.erase(it);
    }
}
ComPtr<ID3D12Resource> PreviewImage12(ID3D12Device* device)
{
    if (!PreviewActive())
    {
        return {};
    }
    std::lock_guard lock(Ledger().mutex);
    const auto it = Ledger().previews.find(device);
    return it == Ledger().previews.end() ? nullptr : it->second->image;
}
HRESULT Capture12(ID3D12GraphicsCommandList* commands, const FrameInput& original, void* stream)
{
    if (!ProcessingActive() || !Enabled())
    {
        return S_FALSE;
    }
    if (!commands)
    {
        return E_POINTER;
    }
    std::lock_guard lock(Ledger().mutex);
    auto& l = Ledger();
    if (!ProcessingActive())
    {
        return S_FALSE;
    }
    try
    {
        auto input = original;
        for (unsigned n = 0; n < 3; ++n)
        {
            if (!input.resources[n])
            {
                input.resourceMetadata[n] = {};
                continue;
            }
            if (!input.resourceMetadata[n].valid)
            {
                input.resourceMetadata[n].description = input.resources[n]->GetDesc();
                input.resourceMetadata[n].valid = true;
            }
        }
        {
            std::lock_guard statusLock(detail::StatusMutex());
            auto& s = detail::StatusBlock();
            s.motionScaleX = input.guides.motionScaleX;
            s.motionScaleY = input.guides.motionScaleY;
            for (unsigned n = 0; n < 3; ++n)
            {
                s.resources[n] = {};
                if (!input.resources[n])
                {
                    continue;
                }
                const auto& d = input.resourceMetadata[n].description;
                s.resources[n] = {unsigned(d.Width), d.Height, unsigned(d.Format), true};
            }
        }
        Collect();
        const auto identity = ListIdentity(commands);
        if (l.pending.contains(identity.Get()) || l.pending.size() + l.submitted.size() >= 4)
        {
            // A skipped frame leaves the model's history untouched; forcing a Reset here would
            // throw away accumulation every time the queue runs ahead of the GPU.
            if (ModelActive())
            {
                PublishCaptureBusy();
            }
            return S_FALSE;
        }
        // One preview mode exists: every input, side by side. The panel offers no other choice.
        const bool preview = PreviewActive();
        bool model = ModelActive();
        if (!input.resources[0])
        {
            const auto hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            PublishCaptureFailure(hr, "Selected resource missing; original game frame preserved");
            return hr;
        }
        ComPtr<ID3D12Device> device;
        auto hr = commands->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(hr))
        {
            throw hr;
        }
        if (model)
        {
            const auto init = Runtime().Initialize(device.Get());
            if (FAILED(init))
            {
                model = false;
                if (!preview)
                {
                    PublishCaptureFailure(
                        init, "NR runtime initialization failed; original game frame preserved");
                    return init;
                }
            }
        }
        // Runtime init has no command list. Snapshot before any feature/GPU work.
        // A late hook cannot reconstruct earlier binds; wait for a tracked Reset.
        std::unique_ptr<CommandStateScope> bindings;
        if (model)
        {
            bindings = std::make_unique<CommandStateScope>(commands);
            if (!*bindings)
            {
                PassStatus status;
                status.error = S_FALSE;
                status.frameSkipped = true;
                status.hasInput = true;
                status.inputTime = GetTickCount64();
                status.reason = bindings->Reason();
                PublishPass(status);
                return S_FALSE;
            }
        }
        if (model && commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            throw E_NOTIMPL;
        }
        auto job = AcquireCopy(device.Get());
        if (!job)
        {
            // No slot is a timing miss, not a reason to discard the model's history.
            if (model)
            {
                PublishCaptureBusy();
            }
            return S_FALSE;
        }

        job->ReleaseFrame();
        job->frame.guides = input.guides;
        job->frame.ticket = std::make_shared<GpuTicket>();
        struct PreparationGuard
        {
            Captured12& job;
            bool registered{};
            ~PreparationGuard()
            {
                if (!registered)
                {
                    job.frame.ticket->Discard();
                    job.ReleaseFrame();
                }
            }
        } preparation{*job};
        job->recordedCopyStates = job->copyStates;
        job->recordedExposureState = job->exposureCopyState;
        std::array<bool, 3> direct{};
        const bool directResolve = model && DirectResolve();
        if (directResolve)
        {
            if (input.states[0] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                throw E_INVALIDARG;
            }
            job->frame.resolveTarget = input.resources[0];
            job->frame.resolveTargetMetadata = input.resourceMetadata[0];
        }
        bool directExposure{};
        InputReadStates12 reads(commands);
        for (unsigned n = 0; n < 3; ++n)
        {
            // The preview shows all three, so either demand needs every input.
            const bool needed = model || preview;
            if (!input.resources[n] || !needed)
            {
                continue;
            }
            ComPtr<ID3D12Device> owner;
            ThrowIfFailed(input.resources[n]->GetDevice(IID_PPV_ARGS(&owner)));
            auto d = input.resourceMetadata[n].description;
            if (owner.Get() != device.Get())
            {
                throw E_INVALIDARG;
            }
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 ||
                d.MipLevels < 1 || d.SampleDesc.Count != 1)
            {
                throw E_NOTIMPL;
            }
            const auto shaderStates = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if ((input.states[n] & shaderStates) &&
                (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            {
                throw E_NOTIMPL;
            }
            if ((input.states[n] & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            {
                throw E_NOTIMPL;
            }
            job->borrowed[n] = input.resources[n];
            // directResolve requires every input to be copied: measured on Death Stranding DC,
            // reading depth/motion in place reproduces the failure that direct mode exists to
            // avoid, even though neither can alias the resolve destination.
            direct[n] = model && !directResolve && DirectShaderInput12(d, n == 1);
            if (direct[n])
            {
                job->frame.resources[n] = input.resources[n];
                job->frame.resourceMetadata[n] = {d, true};
                reads.Add(input.resources[n], input.states[n]);
                continue;
            }
            d.Flags = D3D12_RESOURCE_FLAG_NONE;
            d.Format = StorageFormat(d.Format);
            d.Alignment = 0;
            d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            // Only subresource 0 is read by the model; a full mip chain on the game's
            // target does not need to be replicated.
            d.MipLevels = 1;
            EnsureCopy(device.Get(), d, job->copies[n], job->copyDescs[n], job->copyStates[n]);
            job->recordedCopyStates[n] = job->copyStates[n];
            job->frame.resources[n] = job->copies[n];
            job->frame.resourceMetadata[n] = {d, true};
        }
        if (model && input.exposureResource)
        {
            auto d = input.exposureMetadata.valid ? input.exposureMetadata.description
                                                  : input.exposureResource->GetDesc();
            ComPtr<ID3D12Device> owner;
            ThrowIfFailed(input.exposureResource->GetDevice(IID_PPV_ARGS(&owner)));
            if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == 1 &&
                d.Height == 1 && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
                d.SampleDesc.Count == 1 && owner.Get() == device.Get() &&
                ShaderViewCompatible(d.Format))
            {
                job->borrowedExposure = input.exposureResource;
                // The preview reads the same resources on this list, so nothing about it changes
                // where an input is read from.
                directExposure = DirectShaderInput12(d);
                if (directExposure)
                {
                    job->frame.exposureTexture = input.exposureResource;
                    job->frame.exposureMetadata = {d, true};
                    reads.Add(input.exposureResource, input.exposureState);
                }
                else
                {
                    d.Flags = D3D12_RESOURCE_FLAG_NONE;
                    d.Alignment = 0;
                    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                    d.Format = StorageFormat(d.Format);
                    EnsureCopy(device.Get(), d, job->exposureCopy, job->exposureDesc,
                               job->exposureCopyState);
                    job->recordedExposureState = job->exposureCopyState;
                    job->frame.exposureTexture = job->exposureCopy;
                    job->frame.exposureMetadata = {d, true};
                }
            }
        }
        // Register before touching the game's recording. Execute/Reset lifecycle is required.
        InstallCommandState(commands);
        auto* cookie = new RecordingCookie(identity.Get());
        hr = commands->SetPrivateDataInterface(RecordingCookieId, cookie);
        cookie->Release();
        if (FAILED(hr))
        {
            throw hr;
        }
        l.pending.emplace(identity.Get(), job);
        job->list = commands;
        pendingCount = l.pending.size();
        preparation.registered = true;
        // Every input copy shares one barrier pair: the copies no longer serialize
        // on their own transitions. n == 3 is the exposure copy.
        auto eachCopy = [&](auto&& f)
        {
            for (unsigned n = 0; n < 3; ++n)
            {
                if (job->borrowed[n] && !direct[n])
                {
                    f(n, input.resources[n], input.states[n], job->frame.resources[n].Get(),
                      job->copyStates[n]);
                }
            }
            if (job->frame.exposureTexture && !directExposure)
            {
                f(3u, input.exposureResource, input.exposureState, job->frame.exposureTexture.Get(),
                  job->exposureCopyState);
            }
        };
        BarrierBatch12 copies(commands);
        eachCopy([&](unsigned, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
                     ID3D12Resource* copy, D3D12_RESOURCE_STATES copyState)
                 {
                     copies.AddSubresource(copy, 0, copyState, D3D12_RESOURCE_STATE_COPY_DEST);
                     copies.AddSubresource(source, 0, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                 });
        copies.Flush();
        eachCopy([&](unsigned n, ID3D12Resource* source, D3D12_RESOURCE_STATES,
                     ID3D12Resource* copy, D3D12_RESOURCE_STATES)
                 {
                     CopySubresource0(commands, copy, source);
                     if (n < 3)
                     {
                         job->recordedCopyStates[n] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                     }
                     else
                     {
                         job->recordedExposureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                     }
                 });
        eachCopy([&](unsigned, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
                     ID3D12Resource* copy, D3D12_RESOURCE_STATES)
                 {
                     copies.AddSubresource(source, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
                     copies.AddSubresource(copy, 0, D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                 });
        copies.Flush();
        reads.Begin();
        // The preview is recorded here, on the game's own list, while its inputs are readable and
        // before the game may reuse them. Recording it later, on the panel's list, required private
        // copies of every input and a captured slot held for the panel; that exhausted the copy
        // pool and dropped NR for as long as a preview was selected.
        if (preview)
        {
            try
            {
                auto& slot = l.previews[device.Get()];
                if (!slot)
                {
                    slot = std::make_shared<Preview12>(device.Get());
                }
                auto previewFrame = job->frame;
                previewFrame.guides.displayScale = DisplayScale();
                previewFrame.guides.depthContrast = DepthContrast();
                const auto previewResult = slot->Record(commands, previewFrame);
                if (FAILED(previewResult))
                {
                    slot->image.Reset();
                }
            }
            catch (...)
            {
            } // an optional preview must never disable a healthy model
        }
        if (model)
        {
            if (l.modelRevision != ModelRevision())
            {
                l.model12.clear();
                l.spare12.clear();
                l.modelRevision = ModelRevision();
            }
            const auto key = std::make_pair(device.Get(), stream);
            auto settings = GetModelSettings();
            if (!l.model12.contains(key) && l.model12.size() >= 4)
            {
                reads.Restore();
                PassStatus status;
                status.error = S_FALSE;
                status.frameSkipped = true;
                status.hasInput = true;
                status.reason =
                    "NR stream limit (4) reached; this stream skipped; original frame preserved";
                PublishPass(status);
            }
            else
            {
                auto& pass = l.model12[key];
                if (!pass)
                {
                    if (!l.spare12.empty())
                    {
                        pass = std::move(l.spare12.back());
                        l.spare12.pop_back();
                        pass->InvalidateHistory(L"a parked executor is reused");
                    }
                    else
                    {
                        pass = std::make_unique<PassExecutor12>(device.Get(), Runtime());
                    }
                }
                modelCached = true;
                ID3D12Resource* output{};
                pass->Record(commands, job->frame, input.exposure, settings.pass, input.reset,
                             job->modelRecording, output);
                const auto snapshot = pass->Snapshot();
                PublishPass(snapshot);
                reads.Restore();
                if (snapshot.edited && output && output != input.resources[0])
                {
                    CopyNrResult12(commands, output, input.resources[0], input.states[0]);
                }
            }
        }
        return S_OK;
    }
    catch (HRESULT hr)
    {
        PublishCaptureFailure(hr, hr == E_NOTIMPL    ? "Unsupported texture or view format; original frame preserved"
                                  : hr == E_INVALIDARG ? "Invalid resource, dimensions or subrect; original frame preserved"
                                                       : "NR resource capture failed; original frame preserved");
        return hr;
    }
    catch (...)
    {
        PublishCaptureFailure(E_FAIL, "NR resource capture failed; original frame preserved");
        return E_FAIL;
    }
}
HRESULT PrepareSubmission(ID3D12CommandQueue* queue, UINT count,
                          ID3D12CommandList* const* lists) noexcept
{
    if (!Pending())
    {
        return S_OK;
    }
    try
    {
        std::lock_guard lock(Ledger().mutex);
        auto& l = Ledger();
        // Allocate retirement capacity before any native submission.
        l.submitted.reserve(l.submitted.size() + count);
        for (UINT n = 0; n < count; ++n)
        {
            const auto it = PendingJob(l.pending, lists[n]);
            if (it == l.pending.end())
            {
                continue;
            }
            const auto hr = it->second->frame.ticket->PrepareSubmission(queue);
            if (FAILED(hr))
            {
                PassStatus failure;
                failure.error = hr;
                failure.reason = "NR queue dependency failed; affected batch not submitted";
                PublishPass(failure);
                return hr;
            }
        }
        return S_OK;
    }
    catch (...)
    {
        return E_FAIL;
    }
}
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept
{
    if (!Pending())
    {
        return;
    }
    try
    {
        std::lock_guard lock(Ledger().mutex);
        auto& l = Ledger();
        for (UINT n = 0; n < count; ++n)
        {
            auto it = PendingJob(l.pending, lists[n]);
            if (it == l.pending.end())
            {
                continue;
            }
            auto job = it->second;
            job->list.Reset();
            job->copyStates = job->recordedCopyStates;
            job->exposureCopyState = job->recordedExposureState;
            const auto next = job->completion + 1;
            const auto hr = queue->Signal(job->fence.Get(), next);
            if (SUCCEEDED(hr))
            {
                job->completion = next;
            }
            else
            {
                job->completion = 0;
            } // unretirable work stays strongly held
            job->frame.ticket->Submit(SUCCEEDED(hr) ? job->fence.Get() : nullptr,
                                      SUCCEEDED(hr) ? job->completion : 0, queue);
            l.submitted.push_back(job);
            l.pending.erase(it);
            submittedCount = l.submitted.size();
        }
        pendingCount = l.pending.size();
        Collect();
    }
    catch (...)
    {
    } // the original ExecuteCommandLists has already run
}
void Discard(ID3D12GraphicsCommandList* list) noexcept
{
    // Called for every game command-list Reset; stay lock-free while nothing is pending.
    if (!Pending())
    {
        return;
    }
    try
    {
        const auto identity = ListIdentity(list);
        DiscardIdentity(identity.Get());
    }
    catch (...)
    {
    }
}
namespace
{
void DiscardIdentity(IUnknown* identity) noexcept
{
    if (!Pending())
    {
        return;
    }
    try
    {
        std::lock_guard lock(Ledger().mutex);
        auto& pending = Ledger().pending;
        const auto it = pending.find(identity);
        if (it == pending.end())
        {
            return;
        }
        auto& job = *it->second;
        job.frame.ticket->Discard();
        job.list.Reset();
        job.ReleaseFrame();
        pending.erase(it);
        pendingCount = pending.size();
    }
    catch (...)
    {
    }
}
}
}
