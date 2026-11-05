// SPDX-License-Identifier: GPL-3.0-or-later
// COM forwarding adapted from the earlier Lite build/src/panel_internal.hpp and capture.cpp.
// Only the swapchain is wrapped. The factory object is never replaced: its native
// CreateSwapChain* entries are detoured instead, because handing the game a foreign
// IDXGIFactory7 makes its own DXGI calls fail (Alan Wake 2 device loss, 0x887A002B).
#pragma once
#include "graphics.hpp"

#include <atomic>
#include <new>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace litedx12::capture {
inline constexpr GUID SwapchainIID={0xd2179f03,0xd92b,0x4785,{0xb1,0x13,0x2c,0x91,0x81,0x05,0x80,0x11}};
void WrapSwapchainObject(IUnknown* device, IDXGISwapChain** output,UINT requestedWidth,UINT requestedHeight) noexcept;
void ResolveSwapchainRequest(HWND,UINT& width,UINT& height) noexcept;
// Native DXGI/D3D creation must keep its own internal factory objects.
struct NativeFactoryScope { NativeFactoryScope(); ~NativeFactoryScope(); NativeFactoryScope(const NativeFactoryScope&)=delete;NativeFactoryScope& operator=(const NativeFactoryScope&)=delete; };
HRESULT ForwardPresent(IDXGISwapChain4*,ID3D12CommandQueue*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*,bool);
template<class T> void WrapSwapchain(IUnknown* device, T** output, UINT requestedWidth, UINT requestedHeight) noexcept {
    WrapSwapchainObject(device,reinterpret_cast<IDXGISwapChain**>(output),requestedWidth,requestedHeight);
}
class Swapchain final : public IDXGISwapChain4 {
public:
 HRESULT ResizeWithQueues(UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*);
 Swapchain(IDXGISwapChain4* native,ID3D12CommandQueue* queue):real(native),commandQueue(queue){
 }
 ~Swapchain(){
  litedx12::ReleaseSwapchain(this);
 }
 ComPtr<IDXGISwapChain4> real;
 ComPtr<ID3D12CommandQueue> commandQueue;
 std::atomic<ULONG> refs{1};
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
  if(!out)return E_POINTER;*out=nullptr;
  // The native chain answered SwapChain4 and creation supplied a D3D12 queue.
  if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)||
     iid==__uuidof(IDXGISwapChain)||iid==__uuidof(IDXGISwapChain1)||iid==__uuidof(IDXGISwapChain2)||
     iid==__uuidof(IDXGISwapChain3)||iid==__uuidof(IDXGISwapChain4)||iid==SwapchainIID){
   *out=static_cast<IDXGISwapChain4*>(this);AddRef();return S_OK;
  }
  return real->QueryInterface(iid,out);
 }
 ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override {const ULONG left=--refs;if(!left)delete this;return left;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n,UINT s,const void* d) override {return real->SetPrivateData(n,s,d);}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n,const IUnknown* p) override {return real->SetPrivateDataInterface(n,p);}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n,UINT* s,void* d) override {return real->GetPrivateData(n,s,d);}
 HRESULT STDMETHODCALLTYPE GetParent(REFIID i,void** p) override {return real->GetParent(i,p);}
 HRESULT STDMETHODCALLTYPE GetDevice(REFIID i,void** p) override {return real->GetDevice(i,p);}
 HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override {
  if(!(flags&DXGI_PRESENT_TEST))litedx12::DrawSwapchain(this,flags);
  HRESULT hr;
  hr=ForwardPresent(real.Get(),commandQueue.Get(),sync,flags,nullptr,false);
  if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET||hr==DXGI_ERROR_DEVICE_HUNG)litedx12::FailSwapchain(this,hr);
  return hr;
 }
 HRESULT STDMETHODCALLTYPE GetBuffer(UINT n,REFIID i,void** p) override {return real->GetBuffer(n,i,p);}
 HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f,IDXGIOutput* p) override {
  HRESULT hr;{NativeFactoryScope internal;hr=real->SetFullscreenState(f,p);}
  return hr;
 }
 HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f,IDXGIOutput** p) override {NativeFactoryScope internal;return real->GetFullscreenState(f,p);}
 HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* p) override {NativeFactoryScope internal;return real->GetDesc(p);}
 HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags) override {
  std::lock_guard lock(litedx12::GraphicsMutex());
  // The panel draws into these buffers, so its own objects have to be gone before the engine replaces them,
  // and the present thread must not be inside them while that happens.
  const auto prepared=litedx12::ResizeSwapchain(this,true,S_OK);
  if(FAILED(prepared))return prepared;
  HRESULT hr;
  {NativeFactoryScope internal;hr=real->ResizeBuffers(n,w,h,f,flags);}
  litedx12::ResizeSwapchain(this,false,hr);return hr;
 }
 HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* p) override {
  HRESULT hr;{NativeFactoryScope internal;hr=real->ResizeTarget(p);}
  return hr;
 }
 HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** p) override {return real->GetContainingOutput(p);}
 HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* p) override {return real->GetFrameStatistics(p);}
 HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* p) override {return real->GetLastPresentCount(p);}
 HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* p) override {NativeFactoryScope internal;return real->GetDesc1(p);}
 HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p) override {NativeFactoryScope internal;return real->GetFullscreenDesc(p);}
 HRESULT STDMETHODCALLTYPE GetHwnd(HWND* p) override {return real->GetHwnd(p);}
 HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID i,void** p) override {return real->GetCoreWindow(i,p);}
 HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* p) override {
  if(!(flags&DXGI_PRESENT_TEST))litedx12::DrawSwapchain(this,flags);
  // Baseline behaviour: the game's own dirty and scroll rectangles are forwarded unchanged.
  const DXGI_PRESENT_PARAMETERS* forwarded=p;
  HRESULT hr;
  hr=ForwardPresent(real.Get(),commandQueue.Get(),sync,flags,forwarded,true);
  if(hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET||hr==DXGI_ERROR_DEVICE_HUNG)litedx12::FailSwapchain(this,hr);
  return hr;
 }
 BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {return real->IsTemporaryMonoSupported();}
 HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** p) override {return real->GetRestrictToOutput(p);}
 HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* p) override {return real->SetBackgroundColor(p);}
 HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* p) override {return real->GetBackgroundColor(p);}
 HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override {return real->SetRotation(r);}
 HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* p) override {return real->GetRotation(p);}
 HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w,UINT h) override {return real->SetSourceSize(w,h);}
 HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w,UINT* h) override {return real->GetSourceSize(w,h);}
 HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT n) override {return real->SetMaximumFrameLatency(n);}
 HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* p) override {return real->GetMaximumFrameLatency(p);}
 HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {return real->GetFrameLatencyWaitableObject();}
 HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* p) override {return real->SetMatrixTransform(p);}
 HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* p) override {return real->GetMatrixTransform(p);}
 UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {return real->GetCurrentBackBufferIndex();}
 HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c,UINT* p) override {return real->CheckColorSpaceSupport(c,p);}
 HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override {
  return real->SetColorSpace1(c);
 }
 HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags,const UINT* masks,IUnknown* const* queues) override {
  return ResizeWithQueues(n,w,h,f,flags,masks,queues);
 }
 HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t,UINT s,void* p) override {return real->SetHDRMetaData(t,s,p);}

};

}
