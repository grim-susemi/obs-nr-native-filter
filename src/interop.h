// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <memory>
#include <span>
namespace nr::interop {
enum class Completion { Pending, Complete, Failed };
inline Completion completion(uint64_t required,uint64_t actual) {
 if(actual==UINT64_MAX)return Completion::Failed;
 return actual>=required?Completion::Complete:Completion::Pending;
}
class FenceCursor {
 uint64_t value_;
public:
 explicit FenceCursor(uint64_t value=0):value_(value){}
 uint64_t reserve(){return value_>=UINT64_MAX-1?0:++value_;}
};
inline bool same_luid(LUID a,LUID b){return a.HighPart==b.HighPart&&a.LowPart==b.LowPart;}
inline DXGI_FORMAT source_format(PixelFormat f){switch(f){
 case PixelFormat::Bgra8:return DXGI_FORMAT_B8G8R8A8_UNORM;
 case PixelFormat::Rgba8:return DXGI_FORMAT_R8G8B8A8_UNORM;
 case PixelFormat::Rgba16Float:return DXGI_FORMAT_R16G16B16A16_FLOAT;
 case PixelFormat::Rgb10A2:return DXGI_FORMAT_R10G10B10A2_UNORM;
 default:return DXGI_FORMAT_UNKNOWN;}}
inline DXGI_FORMAT canonical_format(PixelFormat f){return f==PixelFormat::Bgra8?DXGI_FORMAT_R8G8B8A8_UNORM:source_format(f);}
// OBS's sRGB-capable allocations are typeless. Copy only within the SAME DXGI family;
// BGRA and RGBA remain different families and use the actual shader conversion.
inline bool source_format_compatible(DXGI_FORMAT actual,PixelFormat declared){
 const auto typed=source_format(declared);if(typed==DXGI_FORMAT_UNKNOWN)return false;
 if(actual==typed)return true;
 switch(declared){
 case PixelFormat::Bgra8:return actual==DXGI_FORMAT_B8G8R8A8_TYPELESS;
 case PixelFormat::Rgba8:return actual==DXGI_FORMAT_R8G8B8A8_TYPELESS;
 case PixelFormat::Rgba16Float:return actual==DXGI_FORMAT_R16G16B16A16_TYPELESS;
 case PixelFormat::Rgb10A2:return actual==DXGI_FORMAT_R10G10B10A2_TYPELESS;
 default:return false;}
}
enum class WorkPhase { Empty, Captured, Recording, Submitted, Discarded };
struct Ownership {
 std::optional<JobKey> key;
 WorkPhase phase;
 uint64_t producer,worker,consumer,producerDone,workerDone,consumerDone;
 bool consumerOpen,cancelled,failed;
 uint64_t consumerLease=0; // Acquisition identity, never a completion requirement.
};
inline bool reusable(const Ownership& x){
 if(x.phase==WorkPhase::Empty)return !x.key;
 if(!x.key||x.failed||x.consumerOpen||completion(x.producer,x.producerDone)!=Completion::Complete||
    completion(x.consumer,x.consumerDone)!=Completion::Complete||completion(x.worker,x.workerDone)==Completion::Failed)return false;
 return nr::can_reuse({x.key,x.phase==WorkPhase::Submitted,x.phase==WorkPhase::Discarded,
  true,completion(x.worker,x.workerDone)==Completion::Complete,true});
}
inline bool publishable(const Ownership& x,const JobKey& expected){
 return expected.valid()&&x.key&&*x.key==expected&&x.phase==WorkPhase::Submitted&&!x.failed&&!x.cancelled&&
  completion(x.producer,x.producerDone)==Completion::Complete&&completion(x.worker,x.workerDone)==Completion::Complete&&
  completion(x.consumer,x.consumerDone)!=Completion::Failed;
}
bool same_identity(IUnknown*,IUnknown*);
LUID renderer_luid(ID3D11Device*);
bool compatible_context(ID3D11Device*,ID3D12Device*,ID3D12CommandQueue*);
HRESULT open_shared_resource(ID3D12Device*,HANDLE,ID3D12Resource**);
// One private same-adapter device/queue. Callers retain this object for all external GPU ownership.
// Guides/core receive only explicit device/queue; no renderer immediate-context API is exposed.
class Context final {
 Microsoft::WRL::ComPtr<ID3D11Device5> renderer_;
 Microsoft::WRL::ComPtr<ID3D12Device> device_;
 Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
 DWORD graphicsThread_;
 LUID luid_;
 explicit Context(ID3D11Device*);
 friend class Bridge;
public:
 static std::shared_ptr<Context> create(ID3D11Device*);
 ID3D12Device* device()const{return device_.Get();}
 ID3D12CommandQueue* queue()const{return queue_.Get();}
 LUID luid()const{return luid_;}
 DWORD graphics_thread()const{return graphicsThread_;}
};
struct Ticket { JobKey key; size_t slot; bool operator==(const Ticket&)const=default; };
struct WorkerView {
 ID3D12GraphicsCommandList* commands;
 ID3D12Resource* input;
 ID3D12Resource* output;
 uint64_t completionValue;
};
class Bridge;
struct Consumer {
 const Ticket ticket;
 ID3D11Texture2D* const texture;
 const uint64_t leaseValue;
 const bool raw;
 const Bridge* const bridge;
 const Microsoft::WRL::ComPtr<ID3D11Fence> fence;
 const DWORD graphicsOwner;
};
// Precreated exclusive one-use event. Bridge retains it on binding/Signal failure.
class ConsumerEvent final {
 HANDLE handle_;
 bool bound_=false;
 explicit ConsumerEvent(HANDLE handle):handle_(handle){}
 friend class Bridge;
public:
 static std::shared_ptr<ConsumerEvent> create();
 ~ConsumerEvent();
 ConsumerEvent(const ConsumerEvent&)=delete;
 ConsumerEvent& operator=(const ConsumerEvent&)=delete;
 HANDLE handle()const{return handle_;}
};
struct ConsumerEnd {
 const Consumer consumer;
 const uint64_t completionValue;
 const std::shared_ptr<ConsumerEvent> event;
};
// Own one resource generation, exactly three slots. This is transport, NOT Task10 scheduling.
// Graphics calls are capture/acquire/copy/end/cancel/close. begin/submit/discard are one worker.
// Source texture is borrowed only during capture on the graphics owner thread.
// Worker views enter/leave COMMON. External recording must restore COMMON before submit.
class Bridge final {
 struct Impl;
 std::unique_ptr<Impl> impl_;
 bool closed_=false; // Only an accepted close of the original Impl sets this.
public:
 Bridge(std::shared_ptr<Context>,uint64_t generation,const FrameDescriptor&,
        std::span<const std::byte> vertexShader,std::span<const std::byte> pixelShader);
 ~Bridge();
 Bridge(const Bridge&)=delete;
 Bridge& operator=(const Bridge&)=delete;
 std::optional<Ticket> capture(const JobKey&,ID3D11Texture2D*);
 // Scheduler reservation authority. Invalid/busy refusal changes no ownership or cursor.
 std::optional<Ticket> capture_at(size_t,const JobKey&,ID3D11Texture2D*);
 std::optional<WorkerView> begin_worker(const Ticket&);
 bool submit_worker(const Ticket&);
 bool discard_recording(const Ticket&);
 bool record_copy(const Ticket&); // Same-format canonical copy with explicit COMMON barriers.
 std::optional<Consumer> acquire_raw(const Ticket&);
 std::optional<Consumer> acquire_output(const Ticket&);
 bool owns_consumer(const Consumer&)const; // Exact live acquisition and original provenance.
 bool copy_to(const Consumer&,ID3D11Texture2D*);
 std::shared_ptr<const ConsumerEnd> end_consumer(const Consumer&,std::shared_ptr<ConsumerEvent> = {});
 bool cancel(const Ticket&);
 Ownership ownership(size_t)const;
 bool quiescent()const;
 bool close(); // Never waits. False preserves all physical ownership.
 bool closed()const{return closed_;}
 ID3D12Fence* producer_fence()const;
 ID3D12Fence* worker_fence()const;
 ID3D11Fence* consumer_fence()const;
};
} // namespace nr::interop
