// SPDX-License-Identifier: GPL-3.0-or-later
#include "command_guard.hpp"
#include "../detour_hook.hpp"
#include "../runtime.hpp"
#include "../wrappers.hpp"
#include <wrl/client.h>
#include <map>
#include <mutex>
#include <atomic>
#include <array>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace litedx12::nr {
namespace {
using litedx12::detour::Hook;
struct Argument {
    enum Kind { Empty,Table,Constants,CBV,SRV,UAV } kind{};
    UINT64 address{},written{};
    std::array<UINT,64> words{};
};
struct Root {
    ComPtr<ID3D12RootSignature> signature;
    // Opt tracks used root arguments, rather than 64 x 64 words for every list.
    std::vector<Argument> args;
    void Invalidate() noexcept {
        for(auto& a:args){a.kind=Argument::Empty;a.address=0;a.written=0;}
    }
    Argument& At(UINT index) {if(args.size()<=index)args.resize(index+1);return args[index];}
};
struct State {
    UINT64 epoch{};
    bool reset{},valid=true;
    const char* unavailable="DX12 invalid or incomplete binding capture";
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<ComPtr<ID3D12DescriptorHeap>,2> heaps;
    UINT count{};
    ComPtr<ID3D12Resource> predicate;
    UINT64 predicateOffset{};
    D3D12_PREDICATION_OP predicateOperation{};
    bool renderPass{};
    Root roots[2];
    void Reset(ID3D12PipelineState* p=nullptr) {
        reset=false;valid=true;unavailable="DX12 invalid or incomplete binding capture";
        pipeline=p;for(auto& heap:heaps)heap.Reset();count=0;predicate.Reset();
        predicateOffset=0;predicateOperation={};renderPass=false;
        for(auto& root:roots){root.signature.Reset();root.Invalidate();}
    }
};
// Hooked module is pinned. Cookies may be released during process teardown;
// avoid destroying the registry/mutex first or releasing GPU objects in DllMain.
struct StateBucket {
    std::recursive_mutex mutex;
    std::map<ID3D12GraphicsCommandList*,State> states;
    // Bumped on every structural change, so a thread-local State* can tell whether the node it
    // cached is still the one this bucket holds.
    std::atomic<uint64_t> generation{1};
};
auto& buckets=*new std::array<StateBucket,64>;
StateBucket& Bucket(ID3D12GraphicsCommandList* list) {
    const auto address=reinterpret_cast<size_t>(list)>>4;
    return buckets[(address^(address>>7)^(address>>13))%buckets.size()];
}
// One entry per thread: the node most recently located for the list being recorded.
struct TrackCache {StateBucket* bucket{};ID3D12GraphicsCommandList* list{};State* state{};uint64_t generation{};UINT64 epoch{};};
thread_local TrackCache trackCache;
std::mutex installMutex;
// Low bit is demand; changing demand invalidates every earlier recording.
// Hooks remain installed, so toggling never detaches code used by other threads.
std::atomic<UINT64> trackingEpoch{1};
thread_local unsigned suspended{};
constexpr GUID StateCookieId{0xf7d23812,0xca97,0x4e29,{0xa3,0xb1,0x2d,0x5c,0x37,0x31,0xd5,0x10}};
constexpr GUID IndirectInfoId{0xf7d23813,0xca97,0x4e29,{0xa3,0xb1,0x2d,0x5c,0x37,0x31,0xd5,0x10}};
struct IndirectInfo {
    bool valid{},graphics{};
    struct Change { Argument::Kind kind{}; UINT index{},offset{},count{}; };
    UINT count{};
    std::array<Change,64> changes{};
};
IndirectInfo DescribeIndirect(const D3D12_COMMAND_SIGNATURE_DESC* desc) {
    IndirectInfo result{};
    if(!desc||!desc->pArgumentDescs||!desc->NumArgumentDescs||desc->NumArgumentDescs>128)return result;
    UINT operations{};
    for(UINT i=0;i<desc->NumArgumentDescs;++i){const auto& a=desc->pArgumentDescs[i];IndirectInfo::Change c{};
        switch(a.Type){
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:result.graphics=true;++operations;continue;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:++operations;continue;
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:continue;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:c={Argument::Constants,a.Constant.RootParameterIndex,a.Constant.DestOffsetIn32BitValues,a.Constant.Num32BitValuesToSet};break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT:c={Argument::Constants,a.IncrementingConstant.RootParameterIndex,a.IncrementingConstant.DestOffsetIn32BitValues,1};break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:c={Argument::CBV,a.ConstantBufferView.RootParameterIndex,0,0};break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:c={Argument::SRV,a.ShaderResourceView.RootParameterIndex,0,0};break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:c={Argument::UAV,a.UnorderedAccessView.RootParameterIndex,0,0};break;
        default:return result;
        }
        if(c.index>=64||c.offset>64||c.count>64-c.offset||result.count==64)return result;
        result.changes[result.count++]=c;
    }
    result.valid=operations==1;return result;
}
struct CreateSignature {
    template<class F>static HRESULT Call(F fn,ID3D12Device* device,const D3D12_COMMAND_SIGNATURE_DESC* desc,
        ID3D12RootSignature* root,REFIID iid,void** out){
        const auto hr=fn(device,desc,root,iid,out);
        if(SUCCEEDED(hr)&&out&&*out){ComPtr<ID3D12CommandSignature> signature;
            if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&signature)))){
                const auto info=DescribeIndirect(desc);signature->SetPrivateData(IndirectInfoId,sizeof(info),&info);
            }}
        return hr;
    }
};
struct CreateDevice {
    template<class F>static HRESULT Call(F fn,IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** out){
        HRESULT hr;{litedx12::capture::NativeFactoryScope internal;hr=fn(adapter,level,iid,out);}
        if(SUCCEEDED(hr)&&out&&*out){ComPtr<ID3D12Device> device;
            if(SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&device))))
                try{InstallDeviceCommandState(device.Get());}catch(...){} // preserve native creation; late fallback retries
        }return hr;
    }
};
void CaptureCreatedList(D3D12_COMMAND_LIST_TYPE,void**,bool,ID3D12PipelineState*) noexcept;
struct CreateList {
    template<class F>static HRESULT Call(F fn,ID3D12Device* device,UINT node,D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline,REFIID iid,void** out){
        const auto hr=fn(device,node,type,allocator,pipeline,iid,out);
        if(SUCCEEDED(hr))CaptureCreatedList(type,out,true,pipeline);
        return hr;
    }
};
struct CreateList1 {
    template<class F>static HRESULT Call(F fn,ID3D12Device4* device,UINT node,D3D12_COMMAND_LIST_TYPE type,
        D3D12_COMMAND_LIST_FLAGS flags,REFIID iid,void** out){
        const auto hr=fn(device,node,type,flags,iid,out);
        if(SUCCEEDED(hr))CaptureCreatedList(type,out,false,nullptr);
        return hr;
    }
};
using HCreateSignature=Hook<CreateSignature,HRESULT,ID3D12Device*,const D3D12_COMMAND_SIGNATURE_DESC*,ID3D12RootSignature*,REFIID,void**>;
using HCreateDevice=Hook<CreateDevice,HRESULT,IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**>;
using HCreateList=Hook<CreateList,HRESULT,ID3D12Device*,UINT,D3D12_COMMAND_LIST_TYPE,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**>;
using HCreateList1=Hook<CreateList1,HRESULT,ID3D12Device4*,UINT,D3D12_COMMAND_LIST_TYPE,D3D12_COMMAND_LIST_FLAGS,REFIID,void**>;
void FinishDevice(bool ok){HCreateSignature::Finish(ok);HCreateDevice::Finish(ok);HCreateList::Finish(ok);HCreateList1::Finish(ok);}
class Cookie final:public IUnknown {
    std::atomic<ULONG> refs{1}; ID3D12GraphicsCommandList* list;
public:
    explicit Cookie(ID3D12GraphicsCommandList* p):list(p){}
    ~Cookie(){auto& bucket=Bucket(list);std::lock_guard lock(bucket.mutex);bucket.generation.fetch_add(1,std::memory_order_release);bucket.states.erase(list);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown)return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {auto n=--refs;if(!n)delete this;return n;}
};
State& Obtain(ID3D12GraphicsCommandList* list) {
    auto& bucket=Bucket(list);
    auto& states=bucket.states;
    auto [it,added]=states.try_emplace(list);
    if(added){bucket.generation.fetch_add(1,std::memory_order_release);
        auto* cookie=new Cookie(list);auto hr=list->SetPrivateDataInterface(StateCookieId,cookie);cookie->Release();
        if(FAILED(hr)){states.erase(list);bucket.generation.fetch_add(1,std::memory_order_release);throw hr;}}
    return it->second;
}
// The recording thread is the only writer of a list's shadow state, so a located node needs no
// lock: D3D12 records one list from one thread at a time, and a list is not released while it is
// being recorded. The bucket generation catches an erased node (list destroyed, address reused)
// before the cached pointer is used again; a miss simply falls back to the locked path.
template<class F> void Track(ID3D12GraphicsCommandList* list,F&& f) noexcept {
    const auto epoch=trackingEpoch.load(std::memory_order_acquire);
    if(suspended||!(epoch&1))return;
    auto& bucket=Bucket(list);
    if(trackCache.state && trackCache.bucket==&bucket && trackCache.list==list &&
       trackCache.epoch==epoch &&
       trackCache.generation==bucket.generation.load(std::memory_order_acquire)) {
        f(*trackCache.state);
        return;
    }
    try {
        std::lock_guard lock(bucket.mutex);
        if(trackingEpoch.load(std::memory_order_acquire)!=epoch)return;
        auto& state=Obtain(list);
        if(state.epoch!=epoch){state.Reset();state.epoch=epoch;}
        trackCache.bucket=&bucket;trackCache.list=list;trackCache.state=&state;trackCache.epoch=epoch;
        trackCache.generation=bucket.generation.load(std::memory_order_acquire);
        f(state);
    } catch(...) {
        trackCache.state=nullptr;
        std::lock_guard lock(bucket.mutex);auto it=bucket.states.find(list);
        if(it!=bucket.states.end())it->second.valid=false;
    }
}
void CaptureCreatedList(D3D12_COMMAND_LIST_TYPE type,void** out,bool open,ID3D12PipelineState* pipeline) noexcept {
    if(suspended||type!=D3D12_COMMAND_LIST_TYPE_DIRECT||!out||!*out)return;
    try {
        ComPtr<ID3D12GraphicsCommandList> list;
        if(FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&list))))return;
        InstallCommandState(list.Get());
        // CreateCommandList begins a known recording without requiring Reset.
        // CreateCommandList1 is closed; its first successful Reset starts capture.
        Track(list.Get(),[&](State& s){s.Reset(open?pipeline:nullptr);s.reset=open;});
    }catch(...){
    }
}
struct Reset {
    template<class F>static HRESULT Call(F fn,ID3D12GraphicsCommandList* l,ID3D12CommandAllocator* a,ID3D12PipelineState* p){
        auto hr=fn(l,a,p);if(SUCCEEDED(hr))Track(l,[&](State& s){s.Reset(p);s.reset=true;});return hr;
    }
};
struct Clear {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,ID3D12PipelineState* p){
        fn(l,p);Track(l,[&](State& s){s.Reset(p);s.reset=true;});
    }
};
struct Pipeline {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,ID3D12PipelineState* p){fn(l,p);Track(l,[&](State& s){s.pipeline=p;});}
};
struct Heaps {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,UINT n,ID3D12DescriptorHeap* const* p){
        fn(l,n,p);Track(l,[&](State& s){if(n>2||(n&&!p)){s.valid=false;return;}
            bool changed=s.count!=n;
            for(UINT i=0;i<2;++i)changed|=s.heaps[i].Get()!=(i<n?p[i]:nullptr);
            if(changed)for(auto& root:s.roots)for(auto& arg:root.args)
                if(arg.kind==Argument::Table){arg.kind=Argument::Empty;arg.address=0;arg.written=0;}
            s.count=n;for(UINT i=0;i<2;++i)s.heaps[i]=i<n?p[i]:nullptr;});
    }
};
template<unsigned G>struct Signature {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,ID3D12RootSignature* p){
        fn(l,p);Track(l,[&](State& s){auto& r=s.roots[G];if(r.signature.Get()!=p){r.Invalidate();r.signature=p;}});
    }
};
template<unsigned G,Argument::Kind K>struct Address {
    template<class F,class V>static void Call(F fn,ID3D12GraphicsCommandList* l,UINT i,V v){
        fn(l,i,v);Track(l,[&](State& s){if(i>=64){s.valid=false;return;}auto& a=s.roots[G].At(i);a.written=0;a.kind=K;
            if constexpr(K==Argument::Table)a.address=v.ptr;else a.address=v;});
    }
};
template<unsigned G>void Words(State& s,UINT i,UINT n,const UINT* p,UINT offset){
    if(i>=64||offset>64||n>64-offset||(n&&!p)){s.valid=false;return;}
    auto& a=s.roots[G].At(i);if(a.kind!=Argument::Constants)a.written=0;a.kind=Argument::Constants;
    for(UINT j=0;j<n;++j){a.words[offset+j]=p[j];a.written|=UINT64(1)<<(offset+j);}
}
template<unsigned G>struct Constant {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,UINT i,UINT v,UINT offset){
        fn(l,i,v,offset);Track(l,[&](State& s){Words<G>(s,i,1,&v,offset);});
    }
};
template<unsigned G>struct Constants {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,UINT i,UINT n,const void* p,UINT offset){
        fn(l,i,n,p,offset);Track(l,[&](State& s){Words<G>(s,i,n,static_cast<const UINT*>(p),offset);});
    }
};
// Bundles and raytracing PSOs require additional state not represented by this envelope.
// Refuse this recording until Reset/ClearState rather than guessing the game's state.
struct Bundle {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,ID3D12GraphicsCommandList* b){fn(l,b);Track(l,[](State& s){s.valid=false;s.unavailable="DX12 bundle bindings unknown; NR skipped";});}
};
struct Pipeline1 {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList4* l,ID3D12StateObject* p){fn(l,p);Track(l,[](State& s){s.valid=false;s.unavailable="DX12 raytracing PSO state unknown; NR skipped";});}
};
struct Indirect {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,ID3D12CommandSignature* signature,UINT n,
        ID3D12Resource* args,UINT64 offset,ID3D12Resource* counter,UINT64 counterOffset){
        fn(l,signature,n,args,offset,counter,counterOffset);
        if(suspended||!(trackingEpoch.load(std::memory_order_acquire)&1))return;
        IndirectInfo info{};UINT bytes=sizeof(info);
        const bool known=signature&&SUCCEEDED(signature->GetPrivateData(IndirectInfoId,&bytes,&info))&&bytes==sizeof(info)&&info.valid;
        Track(l,[&](State& s){
            if(!known){s.valid=false;s.unavailable="DX12 indirect command bindings unknown; NR skipped";return;}
            // D3D12 clears only the root arguments modified by the signature after
            // ExecuteIndirect. GPU argument-buffer values must not be guessed/read back.
            auto& root=s.roots[info.graphics?1:0];
            for(UINT i=0;i<info.count;++i){const auto& c=info.changes[i];
                if(c.kind==Argument::Constants){UINT zero[64]{};
                    if(info.graphics)Words<1>(s,c.index,c.count,zero,c.offset);else Words<0>(s,c.index,c.count,zero,c.offset);
                }else{auto& a=root.At(c.index);a.address=0;a.written=0;a.kind=c.kind;}
            }
        });
    }
};
struct Predicate {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList* l,ID3D12Resource* p,UINT64 offset,D3D12_PREDICATION_OP op){
        fn(l,p,offset,op);Track(l,[&](State& s){s.predicate=p;s.predicateOffset=offset;s.predicateOperation=op;});
    }
};
struct BeginPass {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList4* l,UINT n,const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds,D3D12_RENDER_PASS_FLAGS flags){
        fn(l,n,rt,ds,flags);Track(l,[](State& s){s.renderPass=true;});
    }
};
struct EndPass {
    template<class F>static void Call(F fn,ID3D12GraphicsCommandList4* l){fn(l);Track(l,[](State& s){s.renderPass=false;});}
};
using HR=Hook<Reset,HRESULT,ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*>;
using HC=Hook<Clear,void,ID3D12GraphicsCommandList*,ID3D12PipelineState*>;
using HP=Hook<Pipeline,void,ID3D12GraphicsCommandList*,ID3D12PipelineState*>;
using HH=Hook<Heaps,void,ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*>;
using HB=Hook<Bundle,void,ID3D12GraphicsCommandList*,ID3D12GraphicsCommandList*>;
using HP1=Hook<Pipeline1,void,ID3D12GraphicsCommandList4*,ID3D12StateObject*>;
using HI=Hook<Indirect,void,ID3D12GraphicsCommandList*,ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64>;
using HPr=Hook<Predicate,void,ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,D3D12_PREDICATION_OP>;
using HBegin=Hook<BeginPass,void,ID3D12GraphicsCommandList4*,UINT,const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,D3D12_RENDER_PASS_FLAGS>;
using HEnd=Hook<EndPass,void,ID3D12GraphicsCommandList4*>;
template<unsigned G>using HS=Hook<Signature<G>,void,ID3D12GraphicsCommandList*,ID3D12RootSignature*>;
template<unsigned G>using HT=Hook<Address<G,Argument::Table>,void,ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE>;
template<unsigned G,Argument::Kind K>using HA=Hook<Address<G,K>,void,ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS>;
template<unsigned G>using H1=Hook<Constant<G>,void,ID3D12GraphicsCommandList*,UINT,UINT,UINT>;
template<unsigned G>using HN=Hook<Constants<G>,void,ID3D12GraphicsCommandList*,UINT,UINT,const void*,UINT>;
template<unsigned G>void FinishRoot(bool ok){HS<G>::Finish(ok);HT<G>::Finish(ok);H1<G>::Finish(ok);HN<G>::Finish(ok);HA<G,Argument::CBV>::Finish(ok);HA<G,Argument::SRV>::Finish(ok);HA<G,Argument::UAV>::Finish(ok);}
void Finish(bool ok){HR::Finish(ok);HC::Finish(ok);HP::Finish(ok);HH::Finish(ok);HB::Finish(ok);HP1::Finish(ok);HI::Finish(ok);HPr::Finish(ok);HBegin::Finish(ok);HEnd::Finish(ok);FinishRoot<0>(ok);FinishRoot<1>(ok);}
template<class H>bool Stage(ID3D12GraphicsCommandList* l,size_t slot){auto* p=litedx12::detour::Method(l,slot);if(H::Installed(p))return false;H::Stage(p);return true;}
template<unsigned G>void StageRoot(ID3D12GraphicsCommandList* l){
    Stage<HS<G>>(l,29+G);Stage<HT<G>>(l,31+G);Stage<H1<G>>(l,33+G);Stage<HN<G>>(l,35+G);
    Stage<HA<G,Argument::CBV>>(l,37+G);Stage<HA<G,Argument::SRV>>(l,39+G);Stage<HA<G,Argument::UAV>>(l,41+G);
}
void RestoreRoot(ID3D12GraphicsCommandList* l,const Root& r,bool graphics){
    if(graphics)l->SetGraphicsRootSignature(r.signature.Get());else l->SetComputeRootSignature(r.signature.Get());
    for(UINT i=0;i<r.args.size();++i){const auto& a=r.args[i];switch(a.kind){
    case Argument::Table:if(graphics)l->SetGraphicsRootDescriptorTable(i,{a.address});else l->SetComputeRootDescriptorTable(i,{a.address});break;
    case Argument::CBV:if(graphics)l->SetGraphicsRootConstantBufferView(i,a.address);else l->SetComputeRootConstantBufferView(i,a.address);break;
    case Argument::SRV:if(graphics)l->SetGraphicsRootShaderResourceView(i,a.address);else l->SetComputeRootShaderResourceView(i,a.address);break;
    case Argument::UAV:if(graphics)l->SetGraphicsRootUnorderedAccessView(i,a.address);else l->SetComputeRootUnorderedAccessView(i,a.address);break;
    case Argument::Constants:
        for(UINT j=0;j<64;){if(!(a.written&(UINT64(1)<<j))){++j;continue;}UINT begin=j;
            while(j<64&&(a.written&(UINT64(1)<<j)))++j;
            if(graphics)l->SetGraphicsRoot32BitConstants(i,j-begin,a.words.data()+begin,begin);
            else l->SetComputeRoot32BitConstants(i,j-begin,a.words.data()+begin,begin);}
        break;
    default:break;
    }}
}
}
void SetCommandStateEnabled(bool enabled) noexcept {
    auto current=trackingEpoch.load(std::memory_order_relaxed);
    for(;;){
        if(bool(current&1)==enabled)return;
        if(trackingEpoch.compare_exchange_weak(current,current+1,std::memory_order_release,std::memory_order_relaxed))return;
    }
}
void InstallDeviceCommandState(ID3D12Device* device){
    if(!device)return;
    std::lock_guard lock(installMutex);
    auto* target=litedx12::detour::Method(device,41);
    auto* create=litedx12::detour::Method(device,12);
    ComPtr<ID3D12Device4> extended;device->QueryInterface(IID_PPV_ARGS(&extended));
    auto* create1=extended?litedx12::detour::Method(extended.Get(),51):nullptr;
    if(HCreateSignature::Installed(target)&&HCreateList::Installed(create)&&(!create1||HCreateList1::Installed(create1)))return;
    litedx12::detour::Transaction<&FinishDevice> transaction;HCreateSignature::Stage(target);HCreateList::Stage(create);
    if(create1)HCreateList1::Stage(create1);
    transaction.Commit();
}
void InstallD3D12CommandState(HMODULE module){
    auto* target=reinterpret_cast<void*>(GetProcAddress(module,"D3D12CreateDevice"));
    if(!target)throw HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    std::lock_guard lock(installMutex);if(HCreateDevice::Installed(target))return;
    litedx12::detour::Transaction<&FinishDevice> transaction;HCreateDevice::Stage(target);transaction.Commit();
}
void InstallCommandState(ID3D12GraphicsCommandList* list){
    if(!list)return;
    ComPtr<ID3D12Device> device;if(SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device))))InstallDeviceCommandState(device.Get());
    std::lock_guard lock(installMutex);
    ComPtr<ID3D12GraphicsCommandList4> extended;
    list->QueryInterface(IID_PPV_ARGS(&extended));
    if(HH::Installed(litedx12::detour::Method(list,28))&&
        (!extended||HP1::Installed(litedx12::detour::Method(extended.Get(),75))))return;
    litedx12::detour::Transaction<&Finish> transaction;
    Stage<HR>(list,10);Stage<HC>(list,11);Stage<HP>(list,25);Stage<HB>(list,27);Stage<HH>(list,28);Stage<HI>(list,59);
    Stage<HPr>(list,55);
    StageRoot<0>(list);StageRoot<1>(list);
    if(extended){Stage<HP1>(extended.Get(),75);Stage<HBegin>(extended.Get(),68);Stage<HEnd>(extended.Get(),69);}
    transaction.Commit();
}
struct CommandStateScope::Saved {ID3D12GraphicsCommandList* list;State state;};
CommandStateScope::CommandStateScope(ID3D12GraphicsCommandList* list){
    const auto epoch=trackingEpoch.load(std::memory_order_acquire);
    if(!(epoch&1)){reason="DX12 NR binding tracking inactive";return;}
    InstallCommandState(list);
    auto& bucket=Bucket(list);std::lock_guard lock(bucket.mutex);auto it=bucket.states.find(list);
    if(it==bucket.states.end()||it->second.epoch!=epoch||trackingEpoch.load(std::memory_order_acquire)!=epoch||!it->second.reset){reason="DX12 waiting for a tracked command list Reset";return;}
    if(!it->second.valid){reason=it->second.unavailable;return;}
    // A Reset alone cannot establish the compute bindings used by the game.
    // Opt's NR envelope likewise refuses a frame with no captured root signature
    // where its restore requirement is enabled. Do not replace an uncaptured
    // bindless root with NR's bindings (or nullptr) unless this game declares
    // that it never binds one (Diagnostics.RequireComputeSignature=0).
    if(!it->second.roots[0].signature&&litedx12::RequireComputeSignature()){reason="DX12 waiting for a captured compute root signature";return;}
    // A tracked Reset(nullptr)/ClearState(nullptr) has a known unset PSO.
    // No draw/dispatch can legally depend on it: the caller must bind a PSO
    // before its next execution. Opt likewise restores a PSO only when present.
    // Missing capture is still rejected by the epoch/reset/valid checks above.
    if(it->second.renderPass){reason="DX12 render pass active; NR skipped";return;}
    if(suspended){reason="DX12 nested processing; NR skipped";return;}
    saved=std::make_unique<Saved>(Saved{list,it->second});++suspended;
    list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
}
CommandStateScope::~CommandStateScope(){
    if(!saved)return;auto* list=saved->list;const auto& s=saved->state;
    ID3D12DescriptorHeap* heaps[2]{s.heaps[0].Get(),s.heaps[1].Get()};
    list->SetDescriptorHeaps(s.count,heaps);
    RestoreRoot(list,s.roots[0],false);RestoreRoot(list,s.roots[1],true);
    if(s.pipeline)list->SetPipelineState(s.pipeline.Get());
    list->SetPredication(s.predicate.Get(),s.predicateOffset,s.predicateOperation);--suspended;
}
}
