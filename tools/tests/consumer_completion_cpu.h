// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// CPU adaptations for the verbatim consumer transition extraction.
// Public contracts are actual interop.h; FramePipeline links its actual TU.
// No DirectX device, resource, queue, OBS, driver or vendor API is executed.
#include "frame_pipeline.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

inline unsigned consumerChecks=0;
inline void expect_consumer(bool value,const char* why)
{
 ++consumerChecks;if(!value)throw std::runtime_error(why);
}
template<class T>using CpuCom=Microsoft::WRL::ComPtr<T>;
struct CpuConsumerFence final:ID3D11Fence {
 std::atomic<ULONG> refs{1};
 std::atomic<uint64_t> done{0};
 HRESULT bindingResult=S_OK;
 struct Binding {uint64_t value;HANDLE event;};
 std::vector<Binding> bindings;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out)override{*out=nullptr;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs;if(!n)delete this;return n;}
 void STDMETHODCALLTYPE GetDevice(ID3D11Device** out)override{*out=nullptr;}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE CreateSharedHandle(const SECURITY_ATTRIBUTES*,DWORD,LPCWSTR,HANDLE*)override{return E_NOTIMPL;}
 UINT64 STDMETHODCALLTYPE GetCompletedValue()override{return done.load();}
 HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value,HANDLE event)override{
  if(FAILED(bindingResult))return bindingResult;
  bindings.push_back({value,event});
  if(done!=UINT64_MAX&&done>=value)SetEvent(event);
  std::printf("BIND exact_target=%llu event_fresh=true\n",value);return S_OK;
 }
 void acknowledge(uint64_t value){
  done=value;for(const auto& b:bindings)if(value!=UINT64_MAX&&value>=b.value)SetEvent(b.event);
 }
};
inline CpuCom<CpuConsumerFence> cpu_fence(){
 CpuCom<CpuConsumerFence> p;p.Attach(new CpuConsumerFence);return p;
}
// Texture addresses are opaque CPU identities. Extracted transitions never
// dereference them: shape/convert are the explicitly adapted dependencies below.
struct CpuConsumerTexture {
 std::byte identity{};
 ID3D11Texture2D* Get(){return reinterpret_cast<ID3D11Texture2D*>(this);}
 const ID3D11Texture2D* Get()const{return reinterpret_cast<const ID3D11Texture2D*>(this);}
};
struct CpuConsumerGraphics {
 struct Command {bool signal;size_t slot;uint64_t target;CpuConsumerFence* fence;};
 std::vector<Command> commands;
 size_t acknowledged=0;
 uint64_t lastAccepted=0;
 unsigned flushes=0;
 HRESULT signalResult=S_OK;
 void copy(size_t slot){commands.push_back({false,slot,0,nullptr});}
 HRESULT Signal(ID3D11Fence* fence,uint64_t value){
  if(FAILED(signalResult))return signalResult;
  expect_consumer(value>lastAccepted&&value<UINT64_MAX,"exclusive finite signal submission order");
  commands.push_back({true,0,value,static_cast<CpuConsumerFence*>(fence)});lastAccepted=value;return S_OK;
 }
 void Flush(){++flushes;} // Submission only, deliberately does not acknowledge.
 void acknowledge_prefix(size_t prefix){
  expect_consumer(prefix>=acknowledged&&prefix<=commands.size(),"ordered acknowledgement prefix");
  while(acknowledged<prefix){const auto& c=commands[acknowledged++];if(c.signal)c.fence->acknowledge(c.target);}
 }
};
struct CpuConsumerSlot {
 std::atomic<std::shared_ptr<const nr::JobKey>> key;
 std::atomic<nr::interop::WorkPhase> phase{nr::interop::WorkPhase::Empty};
 std::atomic<uint64_t> producer{0},worker{0},consumer{0},consumerLease{0};
 std::atomic<bool> consumerOpen{false},cancelled{false},failed{false};
 bool consumerRaw=false;
 std::shared_ptr<nr::interop::ConsumerEvent> endEvent;
 std::shared_ptr<const nr::interop::ConsumerEnd> endReceipt;
 CpuConsumerTexture raw,output11;
};
struct CpuConsumerOwner {
 const DWORD owner=GetCurrentThreadId();
 DWORD graphics_thread()const{return owner;}
};
struct CpuConsumerRig {
 std::array<CpuConsumerSlot,3> slots;
 CpuCom<CpuConsumerFence> producer11=cpu_fence(),worker12=cpu_fence(),consumer11=cpu_fence();
 CpuConsumerGraphics graphics;
 std::shared_ptr<CpuConsumerOwner> context=std::make_shared<CpuConsumerOwner>();
 uint64_t signalSeed=0,leaseSeed=0;
 bool create=true;
 CpuConsumerRig(){producer11->done=1;worker12->done=1;}
};
inline std::shared_ptr<CpuConsumerRig> nextConsumerRig;
namespace nr::interop {
// Adapted dependencies/storage. Atomic snapshot/find mirror production; only
// resource creation, capture and conversion are CPU substitutes.
struct Bridge::Impl {
 const std::shared_ptr<CpuConsumerRig> rig;
 const std::shared_ptr<CpuConsumerOwner> context;
 const uint64_t generation;
 const FrameDescriptor descriptor;
 CpuCom<CpuConsumerFence> producer11,worker12,consumer11;
 CpuConsumerGraphics* graphics;
 std::array<CpuConsumerSlot,3>& slots;
 FenceCursor consumerCursor,leaseCursor;
 Impl(std::shared_ptr<CpuConsumerRig> r,uint64_t g,const FrameDescriptor& d):
  rig(std::move(r)),context(rig->context),generation(g),descriptor(d),
  producer11(rig->producer11),worker12(rig->worker12),consumer11(rig->consumer11),
  graphics(&rig->graphics),slots(rig->slots),consumerCursor(rig->signalSeed),leaseCursor(rig->leaseSeed){}
 bool on_graphics()const{return GetCurrentThreadId()==context->graphics_thread();}
 CpuConsumerSlot* find(const Ticket& t){if(t.slot>=slots.size()||t.key.resourceGeneration!=generation)return nullptr;
  auto& s=slots[t.slot];const auto k=s.key.load();return k&&*k==t.key?&s:nullptr;}
 Ownership snapshot(size_t index)const{
  if(index>=slots.size())return {std::nullopt,WorkPhase::Captured,0,0,0,0,0,0,false,false,true};
  const auto& s=slots[index];const auto phase=s.phase.load();const auto key=s.key.load();
  return {key?std::optional<JobKey>{*key}:std::nullopt,phase,s.producer.load(),s.worker.load(),s.consumer.load(),
   producer11->GetCompletedValue(),worker12->GetCompletedValue(),consumer11->GetCompletedValue(),
   s.consumerOpen.load(),s.cancelled.load(),s.failed.load(),s.consumerLease.load()};
 }
 bool shape(ID3D11Texture2D* texture,DXGI_FORMAT)const{return texture!=nullptr;}
 void convert(ID3D11Texture2D* from,ID3D11Texture2D*){
  for(size_t i=0;i<slots.size();++i)if(from==slots[i].raw.Get()||from==slots[i].output11.Get()){
   graphics->copy(i);return;
  }
  throw std::runtime_error("CPU conversion unknown original texture");
 }
};
Bridge::Bridge(std::shared_ptr<Context>,uint64_t g,const FrameDescriptor& d,
 std::span<const std::byte>,std::span<const std::byte>){
 expect_consumer(static_cast<bool>(nextConsumerRig),"CPU rig installed before constructor");
 if(nextConsumerRig->create)impl_=std::make_unique<Impl>(nextConsumerRig,g,d);
}
Bridge::~Bridge(){if(impl_&&!quiescent())(void)impl_.release();}
std::optional<Ticket> Bridge::capture_at(size_t index,const JobKey& key,ID3D11Texture2D*){
 if(!impl_||!impl_->on_graphics()||index>=3||!key.valid()||key.resourceGeneration!=impl_->generation||
    !reusable(impl_->snapshot(index)))return {};
 auto& s=impl_->slots[index];s.key=std::make_shared<const JobKey>(key);
 s.phase=WorkPhase::Captured;s.producer=1;s.worker=0;s.consumer=0;s.consumerLease=0;
 return Ticket{key,index};
}
}
// This file is generated through apply_patch from the inspected current source.
#include "consumer_completion_transitions.inc"
