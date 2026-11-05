// SPDX-License-Identifier: GPL-3.0-or-later
// Direct3D 12 only: DXGI factory/swapchain interception, loader monitoring for
// the game's NGX lookups, and the queue Execute/Reset submission tracking.
#include "wrappers.hpp"
#include "bootstrap.hpp"
#include "detour_hook.hpp"
#include "proxy/factory_origin.hpp"
#include "nr/capture12.hpp"
#include "nr/command_guard.hpp"
#include <atomic>
#include <mutex>
#include <vector>
#include <d3d11on12.h>

namespace {
using litedx12::detour::Hook;
using litedx12::detour::Method;
std::recursive_mutex hookMutex;
thread_local unsigned captureDepth{};
thread_local unsigned parentBypass{};
// Native Present runs inside the wrapper; keep the factory wrapper out of anything the
// present path creates (RTSS builds a D3D11-on-12 device from its own Present hook).
using PresentOperation = HRESULT(*)(void*);
HRESULT WithParentBypass(PresentOperation operation, void* argument) {
    ++parentBypass;
    // Restore even when a native SEH exception is caught above the wrapper.
    __try { return operation(argument); }
    __finally { --parentBypass; }
}
// The swapchain's current command queue and its device, recorded when the chain is wrapped --
// the equivalent of OptiScaler's State::currentCommandQueue / currentD3D12Device.
struct CurrentSwapchain {
    std::mutex mutex;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Device> device;
};
CurrentSwapchain& Current() { static auto* s = new CurrentSwapchain; return *s; }
void SetCurrentQueue(ID3D12CommandQueue* queue) {
    if (!queue) return;
    auto& current = Current();
    {
        std::lock_guard lock(current.mutex);
        if (current.queue.Get() == queue) return;
    }
    ComPtr<ID3D12CommandQueue> held = queue;
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return;
    std::lock_guard lock(current.mutex);
    current.queue = held; current.device = device;
}
void InstallRtssOn12(HMODULE);
struct On12 {
    template<class Fn> static HRESULT CallFrom(Fn fn, void* /*caller*/, IUnknown* device, UINT flags, const D3D_FEATURE_LEVEL* levels,
        UINT levelCount, IUnknown* const* queues, UINT queueCount, UINT nodeMask, ID3D11Device** output,
        ID3D11DeviceContext** context, D3D_FEATURE_LEVEL* selected) {
        std::vector<IUnknown*> copy;
        ComPtr<ID3D12CommandQueue> replacement;
        if (queues && queueCount) copy.assign(queues, queues + queueCount);
        // OptiScaler's dedicated RTSS patch: with RTSS loaded, a D3D11-on-12 device built for the
        // current D3D12 device is sometimes handed a command queue it captured earlier; that stale
        // queue crashes later, so use the swapchain's current queue instead.
        {
            auto& current = Current();
            std::lock_guard lock(current.mutex);
            if (!copy.empty() && current.queue && current.device &&
                copy[0] != static_cast<IUnknown*>(current.queue.Get()) &&
                GetModuleHandleW(L"RTSSHooks64.dll") != nullptr &&
                device == static_cast<IUnknown*>(current.device.Get())) {
                static std::atomic<bool> reported{};
                replacement = current.queue;
                copy[0] = replacement.Get();
            }
        }
        litedx12::capture::NativeFactoryScope internal;
        return fn(device, flags, levels, levelCount, copy.empty() ? queues : copy.data(),
                  copy.empty() ? queueCount : static_cast<UINT>(copy.size()), nodeMask, output, context, selected);
    }
};
using HOn12 = Hook<On12, HRESULT, IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*, UINT, UINT, ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*>;
struct Scope { Scope() { ++captureDepth; } ~Scope() { --captureDepth; } };
// Lite/Opt: install related creation hooks in one transaction, check Commit,
// abort incomplete groups. No native Present/ResizeBuffers/Release detours.
struct Create {
    template<class Fn> static HRESULT Call(Fn fn, IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
        UINT width=desc?desc->BufferDesc.Width:0,height=desc?desc->BufferDesc.Height:0;
        litedx12::capture::ResolveSwapchainRequest(desc?desc->OutputWindow:nullptr,width,height);
        litedx12::capture::NativeFactoryScope internal;
        const auto hr = fn(self, device, desc, out); if (SUCCEEDED(hr)) litedx12::capture::WrapSwapchain(device, out,width,height); return hr;
    }
};
struct CreateHwnd {
    template<class Fn> static HRESULT Call(Fn fn, IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** out) {
        UINT width=desc?desc->Width:0,height=desc?desc->Height:0;
        litedx12::capture::ResolveSwapchainRequest(hwnd,width,height);
        litedx12::capture::NativeFactoryScope internal;
        const auto hr = fn(self, device, hwnd, desc, fullscreen, output, out); if (SUCCEEDED(hr)) litedx12::capture::WrapSwapchain(device, out,width,height); return hr;
    }
};
struct CreateCore {
    template<class Fn> static HRESULT Call(Fn fn, IDXGIFactory2* self, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out) {
        const UINT width=desc?desc->Width:0,height=desc?desc->Height:0;
        litedx12::capture::NativeFactoryScope internal;
        const auto hr = fn(self, device, window, desc, output, out); if (SUCCEEDED(hr)) litedx12::capture::WrapSwapchain(device, out,width,height); return hr;
    }
};
struct CreateComposition {
    template<class Fn> static HRESULT Call(Fn fn, IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out) {
        const UINT width=desc?desc->Width:0,height=desc?desc->Height:0;
        litedx12::capture::NativeFactoryScope internal;
        const auto hr = fn(self, device, desc, output, out); if (SUCCEEDED(hr)) litedx12::capture::WrapSwapchain(device, out,width,height); return hr;
    }
};
using HCreate = Hook<Create, HRESULT, IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**>;
using HHwnd = Hook<CreateHwnd, HRESULT, IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**>;
using HCore = Hook<CreateCore, HRESULT, IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**>;
using HComposition = Hook<CreateComposition, HRESULT, IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**>;
struct FactoryExport {
    template<class Fn> static HRESULT CallFrom(Fn fn, void* caller, REFIID iid, void** out) { auto hr=fn(iid,out); if(SUCCEEDED(hr)&&!litedx12::NativeFactoryCaller(caller)) litedx12::AttachFactory(iid,out); return hr; }
};
struct FactoryExport2 {
    template<class Fn> static HRESULT CallFrom(Fn fn, void* caller, UINT flags, REFIID iid, void** out) { auto hr=fn(flags,iid,out); if(SUCCEEDED(hr)&&!litedx12::NativeFactoryCaller(caller)) litedx12::AttachFactory(iid,out); return hr; }
};
using HFactory = Hook<FactoryExport,HRESULT,REFIID,void**>;
using HFactory2 = Hook<FactoryExport2,HRESULT,UINT,REFIID,void**>;
thread_local unsigned loaderDepth{};
void Watch(HMODULE module) {
    if (!module || (reinterpret_cast<ULONG_PTR>(module)&3)) return;
    wchar_t path[1024]{}; if(!GetModuleFileNameW(module,path,1024)) return;
    const auto leaf=wcsrchr(path,L'\\');
    if(_wcsicmp(leaf?leaf+1:path,L"d3d11.dll")==0){
        try { InstallRtssOn12(module); }
        catch(HRESULT error) { litedx12::GraphicsFailure(error,L"RTSS On12 compatibility hook failed; original module load preserved"); }
        catch(...) { litedx12::GraphicsFailure(E_FAIL,L"RTSS On12 compatibility hook failed; original module load preserved"); }
    }else if(_wcsicmp(leaf?leaf+1:path,L"d3d12.dll")==0){
        try { litedx12::nr::InstallD3D12CommandState(module); }
        catch(HRESULT error) { litedx12::GraphicsFailure(error,L"D3D12 command signature capture failed; original module load preserved"); }
        catch(...) { litedx12::GraphicsFailure(E_FAIL,L"D3D12 command signature capture failed; original module load preserved"); }
    }
}
struct LoaderScope { LoaderScope(){++loaderDepth;} ~LoaderScope(){--loaderDepth;} };
struct Load {
    template<class Fn,class Name> static HMODULE Call(Fn fn,Name name) {
        if(loaderDepth) return fn(name);
        LoaderScope guard; auto module=fn(name); Watch(module); return module;
    }
};
struct LoadEx {
    template<class Fn,class Name> static HMODULE Call(Fn fn,Name name,HANDLE file,DWORD flags) {
        if(loaderDepth) return fn(name,file,flags);
        LoaderScope guard; auto module=fn(name,file,flags);
        if(!(flags&(LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE|LOAD_LIBRARY_AS_IMAGE_RESOURCE|DONT_RESOLVE_DLL_REFERENCES))) Watch(module);
        return module;
    }
};
using HLoadA=Hook<Load,HMODULE,LPCSTR>;
using HLoadW=Hook<Load,HMODULE,LPCWSTR>;
using HLoadExA=Hook<LoadEx,HMODULE,LPCSTR,HANDLE,DWORD>;
using HLoadExW=Hook<LoadEx,HMODULE,LPCWSTR,HANDLE,DWORD>;
struct ProcLookup {
    template<class Fn> static FARPROC Call(Fn fn,HMODULE module,LPCSTR name) {
        const auto original=fn(module,name); return litedx12::nr::ResolveNgx(module,name,original);
    }
};
using HProcLookup=Hook<ProcLookup,FARPROC,HMODULE,LPCSTR>;
struct Execute {
    template<class Fn> static void Call(Fn fn,ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
        if(FAILED(litedx12::nr::PrepareSubmission(queue,count,lists)))return;
        fn(queue,count,lists); litedx12::nr::Submitted(queue,count,lists);
    }
};
struct ResetList {
    template<class Fn> static HRESULT Call(Fn fn,ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* state) {
        const auto hr=fn(list,allocator,state); if(SUCCEEDED(hr)) litedx12::nr::Discard(list); return hr;
    }
};
using HExecute=Hook<Execute,void,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*>;
using HResetList=Hook<ResetList,HRESULT,ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*>;
void Finish(bool success) { HOn12::Finish(success); HCreate::Finish(success); HHwnd::Finish(success); HCore::Finish(success); HComposition::Finish(success);
    HFactory::Finish(success); HFactory2::Finish(success); HLoadA::Finish(success); HLoadW::Finish(success); HLoadExA::Finish(success); HLoadExW::Finish(success);
    HExecute::Finish(success); HResetList::Finish(success); HProcLookup::Finish(success); }
using Transaction = litedx12::detour::Transaction<&Finish>;
void Pin() {
    static bool pinned = false; if (pinned) return;
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Pin), &module)) throw HRESULT_FROM_WIN32(GetLastError());
    pinned = true;
}
// Per-frame/per-enumeration installs: the vtable entry is almost always the one
// already detoured, so skip the lock and the Detours transaction entirely.
template<class H> void InstallOnce(std::atomic<void*>& last, void* target) {
    if (!target || last.load(std::memory_order_acquire) == target) return;
    std::lock_guard lock(hookMutex);
    if (!H::Installed(target)) { Pin(); Transaction transaction; H::Stage(target); transaction.Commit(); }
    last.store(target, std::memory_order_release);
}
std::atomic<void*> lastExecute{}, lastReset{}, lastOn12{};
void InstallRtssOn12(HMODULE module) {
    if (!module) return;
    const auto target = reinterpret_cast<void*>(GetProcAddress(module, "D3D11On12CreateDevice"));
    if (!target) return;
    if (lastOn12.load(std::memory_order_acquire) == target) return;
    // The cached detour entry must remain valid for the pinned plugin's lifetime.
    HMODULE retained{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(target), &retained))
        throw HRESULT_FROM_WIN32(GetLastError());
    InstallOnce<HOn12>(lastOn12, target);
    static std::atomic<bool> reported{};
}
}
namespace litedx12::capture {
NativeFactoryScope::NativeFactoryScope(){++parentBypass;}
NativeFactoryScope::~NativeFactoryScope(){--parentBypass;}
HRESULT ForwardPresent(IDXGISwapChain4* native,ID3D12CommandQueue* queue,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters,bool one) {
    // RTSS can load D3D11 inside its first Present through a loader path our
    // LoadLibrary hooks never see. Install the On12 fix before entering RTSS,
    // when the queue belonging to this particular swapchain is still known.
    // Probed once: RTSS is injected before the game's first frame, and a loader
    // lock lookup on every Present is pure cost.
    static std::atomic<int> rtssProbe{}; // 0 unknown, 1 loaded, 2 absent
    if(queue) {
        auto probe=rtssProbe.load(std::memory_order_relaxed);
        if(!probe) {
            probe=GetModuleHandleW(L"RTSSHooks64.dll")?1:2;
            rtssProbe.store(probe,std::memory_order_relaxed);
        }
        if(probe==1) {
            SetCurrentQueue(queue);
            if (!lastOn12.load(std::memory_order_acquire)) try {
                NativeFactoryScope internal;
                auto module=GetModuleHandleW(L"d3d11.dll");
                if(!module)module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                if(module)InstallRtssOn12(module);
            }catch(HRESULT hr){litedx12::GraphicsFailure(hr,L"RTSS On12 installation before Present failed");}
            catch(...){litedx12::GraphicsFailure(E_FAIL,L"RTSS On12 installation before Present failed");}
        }
    }
    struct Arguments {IDXGISwapChain4* native;UINT sync,flags;const DXGI_PRESENT_PARAMETERS* parameters;bool one;} arguments{native,sync,flags,parameters,one};
    return WithParentBypass([](void* argument)->HRESULT {
        const auto& a=*static_cast<Arguments*>(argument);
        return a.one?a.native->Present1(a.sync,a.flags,a.parameters):a.native->Present(a.sync,a.flags);
    },&arguments);
}
void ResolveSwapchainRequest(HWND window,UINT& width,UINT& height) noexcept {
    if(!litedx12::SkipSmallSwapchains())return;
    RECT client{};
    if(window&&(!width||!height)&&GetClientRect(window,&client)) {
        if(!width&&client.right>client.left)width=UINT(client.right-client.left);
        if(!height&&client.bottom>client.top)height=UINT(client.bottom-client.top);
    }
    // Resolve automatic HWND dimensions before creation; never inspect the
    // returned small-chain COM object. Native creation arguments stay unchanged.
}
void WrapSwapchainObject(IUnknown* device, IDXGISwapChain** output,UINT requestedWidth,UINT requestedHeight) noexcept {
    if (!Usable()) return;
    if (!output || !*output || captureDepth >= 4) return;
    if(litedx12::SkipSmallSwapchains()&&((requestedWidth&&requestedWidth<100)||(requestedHeight&&requestedHeight<100))) {
        // Decide from the creation request, before touching the returned native
        // object. HWND automatic sizes are resolved before native creation too.
        return;
    }
    Scope scope;
    try {
        // SwapChain4 is also implemented by D3D11 chains. Only a D3D12
        // creation queue establishes that this chain belongs to this backend.
        ComPtr<ID3D12CommandQueue> queue;
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
        ComPtr<IUnknown> ours;
        if (SUCCEEDED((*output)->QueryInterface(SwapchainIID, &ours))) return;
        if(litedx12::SkipSmallSwapchains()) {
            DXGI_SWAP_CHAIN_DESC desc{};
            HRESULT hr;{NativeFactoryScope internal;hr=(*output)->GetDesc(&desc);}
            // GTA creates an auxiliary small chain (180x44); wrapping it breaks the game.
            // Use resolved native dimensions, so zero-sized requests for the main window are kept.
            if(SUCCEEDED(hr)&&(desc.BufferDesc.Width<100||desc.BufferDesc.Height<100)) {
                return;
            }
        }
        ComPtr<IDXGISwapChain4> native;
        if (FAILED((*output)->QueryInterface(IID_PPV_ARGS(&native)))) { GraphicsFailure(E_NOINTERFACE, L"Swapchain4 unavailable; native swapchain retained"); return; }
        auto* wrapper = new Swapchain(native.Get(), queue.Get());
        (*output)->Release(); *output = wrapper;
        SetCurrentQueue(wrapper->commandQueue.Get());
        TrackSwapchain(wrapper, queue.Get());
    } catch (HRESULT hr) { GraphicsFailure(hr, L"Swapchain wrapper initialization failed"); }
    catch (...) { GraphicsFailure(E_FAIL, L"Swapchain wrapper initialization failed"); }
}
HRESULT Swapchain::ResizeWithQueues(UINT n, UINT w, UINT h, DXGI_FORMAT format, UINT flags, const UINT* masks, IUnknown* const* queues) {
    std::lock_guard lock(GraphicsMutex());
    const auto prepared = ResizeSwapchain(this, true, S_OK);
    if (FAILED(prepared)) return prepared;
    HRESULT hr;
    {NativeFactoryScope internal;hr = real->ResizeBuffers1(n, w, h, format, flags, masks, queues);}
    ComPtr<ID3D12CommandQueue> queue;
    bool unsupported = false;
    if (SUCCEEDED(hr) && queues) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (!n) {NativeFactoryScope internal;if (SUCCEEDED(real->GetDesc(&desc))) n = desc.BufferCount;}
        for (UINT i = 1; i < n; ++i) if (queues[i] != queues[0]) unsupported = true;
        if (!unsupported && n && queues[0]) queues[0]->QueryInterface(IID_PPV_ARGS(&queue));
    }
    ResizeSwapchain(this, false, hr, queue.Get());
    if (unsupported) FailSwapchain(this, E_NOTIMPL);
    else if (queue) { commandQueue = queue; SetCurrentQueue(queue.Get()); }
    return hr;
}
}
namespace litedx12 {
void InstallFrameQueueCapture(ID3D12CommandQueue* queue) {
    if(queue) InstallOnce<HExecute>(lastExecute, Method(queue,10));
    if(queue){ComPtr<ID3D12Device> device;if(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))))nr::InstallDeviceCommandState(device.Get());}
}
void InstallFrameListCapture(ID3D12GraphicsCommandList* list) {
    if(list) InstallOnce<HResetList>(lastReset, Method(list,10));
}
void InstallCapture() {
    wchar_t path[MAX_PATH]{}; if(!GetSystemDirectoryW(path,MAX_PATH)||wcscat_s(path,L"\\dxgi.dll")) throw E_FAIL;
    const auto dxgi=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dxgi) throw HRESULT_FROM_WIN32(GetLastError());
    const auto kernel=GetModuleHandleW(L"kernel32.dll");
    {
        std::lock_guard lock(hookMutex); Pin();
        Transaction transaction;
        HFactory::Stage(reinterpret_cast<void*>(GetProcAddress(dxgi,"CreateDXGIFactory")));
        HFactory::Stage(reinterpret_cast<void*>(GetProcAddress(dxgi,"CreateDXGIFactory1")));
        HFactory2::Stage(reinterpret_cast<void*>(GetProcAddress(dxgi,"CreateDXGIFactory2")));
        HLoadA::Stage(reinterpret_cast<void*>(GetProcAddress(kernel,"LoadLibraryA")));
        HLoadW::Stage(reinterpret_cast<void*>(GetProcAddress(kernel,"LoadLibraryW")));
        HLoadExA::Stage(reinterpret_cast<void*>(GetProcAddress(kernel,"LoadLibraryExA")));
        HLoadExW::Stage(reinterpret_cast<void*>(GetProcAddress(kernel,"LoadLibraryExW")));
        HProcLookup::Stage(reinterpret_cast<void*>(GetProcAddress(kernel,"GetProcAddress")));
        transaction.Commit();
    }
    // EnsureFactoryWatches: pre-existing factories share these native method entries.
    {
        using CreateFn=HRESULT(WINAPI*)(REFIID,void**);
        ComPtr<IDXGIFactory> probe;
        const auto create=reinterpret_cast<CreateFn>(GetProcAddress(dxgi,"CreateDXGIFactory1"));
        const auto hr=create(IID_PPV_ARGS(&probe)); if(FAILED(hr)) throw hr;
    }
    if(const auto module=GetModuleHandleW(L"d3d11.dll"))InstallRtssOn12(module);
    if(const auto module=GetModuleHandleW(L"d3d12.dll"))nr::InstallD3D12CommandState(module);
}
void AttachFactory(REFIID, void** output) noexcept {
    if (!Usable()) return;
    // NativeFactoryScope protects native DXGI/D3D internals, including factory
    // exports re-entered by the creation call, not only adapter GetParent.
    if (parentBypass) return;
    if (!output || !*output || captureDepth >= 4) return;
    Scope scope;
    try {
        // The game keeps the native factory object. Only its CreateSwapChain*
        // entries are detoured, and each of those wraps the chain it returns.
        // Handing the game a foreign IDXGIFactory7 breaks its own DXGI calls
        // (Alan Wake 2 loses the device with 0x887A002B), so nothing is swapped.
        auto* object = static_cast<IUnknown*>(*output);
        ComPtr<IDXGIFactory> factory; if (FAILED(object->QueryInterface(IID_PPV_ARGS(&factory)))) return;
        {
            std::lock_guard lock(hookMutex);
            const auto create = Method(factory.Get(), 10);
            ComPtr<IDXGIFactory2> two;
            factory.As(&two);
            const auto hwnd = two ? Method(two.Get(), 15) : nullptr;
            const auto core = two ? Method(two.Get(), 16) : nullptr;
            const auto composition = two ? Method(two.Get(), 24) : nullptr;
            if (!HCreate::Installed(create) || (hwnd && !HHwnd::Installed(hwnd)) ||
                (core && !HCore::Installed(core)) || (composition && !HComposition::Installed(composition))) {
                Pin(); Transaction transaction;
                HCreate::Stage(create);
                if (two) { HHwnd::Stage(hwnd); HCore::Stage(core); HComposition::Stage(composition); }
                transaction.Commit();
            }
        }
    } catch (HRESULT hr) { GraphicsFailure(hr, L"Factory capture transaction failed; native factory retained"); }
    catch (...) { GraphicsFailure(E_FAIL, L"Factory capture failed; native factory retained"); }
}
}
