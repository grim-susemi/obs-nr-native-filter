// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - This translation unit owns one bounded interprocess lease state
// machine. The assigned source-only handoff permits only fg_ipc.h/.cpp, not
// additional implementation units. Its producer/importer custody transitions
// must be reviewed together; it does not contain scheduling or presentation.
#include "fg_ipc.h"
#include "fg_ipc_sequence.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

namespace nr::fg::ipc {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
constexpr uint64_t magic=0x314350494746524e, max_serial=INT64_MAX/2;
LONG64 load(const volatile LONG64& x){return InterlockedCompareExchange64(const_cast<volatile LONG64*>(&x),0,0);}
void store(volatile LONG64& x,uint64_t v){InterlockedExchange64(&x,static_cast<LONG64>(v));}
Transfer transfer(const Header& h){return static_cast<Transfer>(load(h.transfer));}
bool exchange(Header& h,Transfer from,Transfer to){
 return InterlockedCompareExchange64(&h.transfer,static_cast<LONG64>(to),static_cast<LONG64>(from))==static_cast<LONG64>(from);
}
void check(HRESULT hr,const char* operation){if(FAILED(hr))throw Error(Status::ProtocolError,hr,operation);}
void win(bool ok,const char* operation){if(!ok)throw Error(Status::ProtocolError,HRESULT_FROM_WIN32(GetLastError()),operation);}
bool complete(uint64_t actual,uint64_t required){return actual!=UINT64_MAX&&actual>=required;}
struct Handle {
 HANDLE value=nullptr;
 Handle()=default;
 explicit Handle(HANDLE h):value(h){}
 ~Handle(){if(value)CloseHandle(value);}
 Handle(const Handle&)=delete;
 Handle& operator=(const Handle&)=delete;
 Handle(Handle&& x)noexcept:value(std::exchange(x.value,nullptr)){}
 Handle& operator=(Handle&& x)noexcept{if(value)CloseHandle(value);value=std::exchange(x.value,nullptr);return *this;}
};
struct Mapping {
 Handle handle; Header* header=nullptr;
 ~Mapping(){if(header)UnmapViewOfFile(header);}
 void map(){header=static_cast<Header*>(MapViewOfFile(handle.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Header)));win(header!=nullptr,"MapViewOfFile");}
};
Handle verified(const Peer& peer){
 Handle h;win(DuplicateHandle(GetCurrentProcess(),peer.process,GetCurrentProcess(),&h.value,0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"retain peer");
 const auto actual=identity(h.value);
 if(actual.pid!=peer.pid||actual.creation!=peer.creation||WaitForSingleObject(h.value,0)!=WAIT_TIMEOUT)
  throw Error(Status::WrongPeer,E_INVALIDARG,"peer identity or liveness");
 return h;
}
bool format(DXGI_FORMAT f){
 switch(f){case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM:
 case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R10G10B10A2_UNORM:return true;default:return false;}
}
bool compatible(DXGI_FORMAT actual,DXGI_FORMAT wanted){
 if(actual==wanted)return true;
 switch(wanted){
 case DXGI_FORMAT_R8G8B8A8_UNORM:return actual==DXGI_FORMAT_R8G8B8A8_TYPELESS;
 case DXGI_FORMAT_B8G8R8A8_UNORM:return actual==DXGI_FORMAT_B8G8R8A8_TYPELESS;
 case DXGI_FORMAT_R16G16B16A16_FLOAT:return actual==DXGI_FORMAT_R16G16B16A16_TYPELESS;
 case DXGI_FORMAT_R10G10B10A2_UNORM:return actual==DXGI_FORMAT_R10G10B10A2_TYPELESS;default:return false;
 }
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
 D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
}
}
Error::Error(Status status,HRESULT code,const char* what):std::runtime_error(what),status_(status),code_(code){}
Peer identity(HANDLE h){
 FILETIME created{},exited{},kernel{},user{};win(GetProcessTimes(h,&created,&exited,&kernel,&user)!=FALSE,"GetProcessTimes");
 const auto pid=GetProcessId(h);win(pid!=0,"GetProcessId");return {h,pid,(uint64_t{created.dwHighDateTime}<<32)|created.dwLowDateTime};
}
Status validate(const Header& h,LUID expected){
 if(h.magic!=magic||h.version!=protocol_version||h.bytes!=sizeof(Header))return Status::VersionMismatch;
 if(!h.generation)return Status::ProtocolError;
 return h.luid.LowPart==expected.LowPart&&h.luid.HighPart==expected.HighPart?Status::Ok:Status::AdapterMismatch;
}
std::optional<Frame> snapshot(const WireFrame& wire){
 const auto before=load(wire.sequence);if(before<=0||(before&1))return {};
 std::array<uint64_t,16> words{};for(size_t i=0;i<words.size();++i)words[i]=static_cast<uint64_t>(load(wire.words[i]));
 if(load(wire.sequence)!=before||words[1]!=static_cast<uint64_t>(before)/2)return {};
 const bool split=(words[6]&2)!=0;const auto percent=static_cast<uint32_t>((words[6]>>8)&0xff);
 Frame result{words[0],words[1],words[2],words[3],{static_cast<uint32_t>(words[4]),static_cast<uint32_t>(words[4]>>32),
  static_cast<DXGI_FORMAT>(static_cast<uint32_t>(words[5])),static_cast<DXGI_COLOR_SPACE_TYPE>(words[5]>>32),{},(words[6]&1)!=0,split},words[15],split?percent:50};
 if(!result.generation||!result.shape.width||!result.shape.height||!format(result.shape.format)||
  (words[6]&~uint64_t{0xff03})||(split?percent>100:percent!=0))return {};
 std::memcpy(result.shape.sourceMonitor.data(),words.data()+7,sizeof(result.shape.sourceMonitor));return result;
}
struct Producer::Config {
 Ptr<ID3D11Device5> device;Ptr<ID3D11DeviceContext4> graphics;std::wstring name;Shape shape;LUID adapter;DWORD thread;
};
struct Producer::Impl {
 struct Slot {Ptr<ID3D11Texture2D> texture;Ptr<ID3D11Fence> write,returned;uint64_t target=0;};
 Mapping mapping;Handle peer,frameEvent,controlEvent;
 Ptr<ID3D11Device5> device;Ptr<ID3D11DeviceContext4> graphics;
 std::array<Slot,4> slots;std::array<HANDLE,14> remote{};Offer offer;
 uint64_t serial=0;bool retired=false,uncertain=false;
 DWORD revokeError=ERROR_SUCCESS;
#ifdef NR_FG_IPC_TEST_PROBE
 decltype(&DuplicateHandle) duplicateCloseSource=&DuplicateHandle;
#endif
 static auto& vault(){static std::array<std::shared_ptr<Impl>,generation_limit> entries;return entries;}
 static auto& mutex(){static std::mutex m;return m;}
 bool dead()const{return WaitForSingleObject(peer.value,0)==WAIT_OBJECT_0;}
 bool revoke(){
  auto& h=*mapping.header;
  if(!exchange(h,Transfer::Offered,Transfer::Revoked)){
   switch(transfer(h)){
   case Transfer::Revoked:case Transfer::Initial:break;
   case Transfer::Offered:case Transfer::Importing:case Transfer::Ready:case Transfer::Rejected:case Transfer::Closed:return false;
   default:return false; // A corrupt wire value cannot authorize closing foreign handles.
   }
  }
  for(auto& handle:remote)if(handle){
   // CLOSE_SOURCE consumes the source even when duplication fails. Remove its
   // number before attempting the call; neither failure nor collection may retry it.
   const auto source=std::exchange(handle,nullptr);
   if(!dead()){
    Handle local;
#ifdef NR_FG_IPC_TEST_PROBE
    const auto result=duplicateCloseSource(peer.value,source,GetCurrentProcess(),&local.value,0,FALSE,DUPLICATE_CLOSE_SOURCE|DUPLICATE_SAME_ACCESS);
#else
    const auto result=DuplicateHandle(peer.value,source,GetCurrentProcess(),&local.value,0,FALSE,DUPLICATE_CLOSE_SOURCE|DUPLICATE_SAME_ACCESS);
#endif
    if(!result)revokeError=GetLastError(); // Diagnostic failure, not retained source custody.
   }
  }
  return true;
 }
 bool safe(){
  if(uncertain)return false;
  bool closed=false,revoked=false;
  switch(transfer(*mapping.header)){
  case Transfer::Closed:case Transfer::Rejected:closed=true;break;
  case Transfer::Offered:case Transfer::Revoked:case Transfer::Initial:revoked=revoke();break;
  case Transfer::Importing:case Transfer::Ready:break;
  }
  if(!closed&&!revoked&&!dead())return false;
  for(const auto& slot:slots)if(slot.target&&(!complete(slot.write->GetCompletedValue(),slot.target)||
   (!closed&&!revoked&&!complete(slot.returned->GetCompletedValue(),slot.target))))return false;
  return true;
 }
 static void collect(){for(auto& p:vault())if(p&&p->retired&&p->safe())p.reset();}
 ~Impl(){if(mapping.header)revoke();}
};
#ifdef NR_FG_IPC_TEST_PROBE
Producer::RevokeProbeResult Producer::test_revoke(std::array<HANDLE,14>& remote,decltype(&DuplicateHandle) duplicate){
 Impl fixture;fixture.peer=verified(identity(GetCurrentProcess()));fixture.duplicateCloseSource=duplicate;
 fixture.mapping.handle=Handle(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(Header)),nullptr));
 win(fixture.mapping.handle.value!=nullptr,"revocation probe mapping");fixture.mapping.map();
 store(fixture.mapping.header->transfer,static_cast<uint64_t>(Transfer::Offered));fixture.remote=remote;
 const auto first=fixture.revoke(),second=fixture.revoke();remote=fixture.remote;
 return {first,second,fixture.revokeError};
}
#endif
Producer::Producer(ID3D11Device* device,std::wstring name,const Shape& shape):config_(std::make_unique<Config>()){
 if(!device||name.empty()||!shape.width||!shape.height||shape.width>16384||shape.height>(shape.splitCompare?8192u:16384u)||!format(shape.format))
  throw Error(Status::InvalidFrame,E_INVALIDARG,"producer shape/device/name");
 check(device->QueryInterface(IID_PPV_ARGS(&config_->device)),"ID3D11Device5");Ptr<ID3D11DeviceContext> immediate;
 device->GetImmediateContext(&immediate);check(immediate.As(&config_->graphics),"ID3D11DeviceContext4");
 Ptr<IDXGIDevice> dxgi;Ptr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
 check(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"D3D11 DXGI device");check(dxgi->GetAdapter(&adapter),"D3D11 adapter");
 check(adapter->GetDesc(&desc),"adapter description");
 config_->name=std::move(name);config_->shape=shape;config_->adapter=desc.AdapterLuid;config_->thread=GetCurrentThreadId();
}
Producer::~Producer(){std::lock_guard lock(Impl::mutex());if(current_)current_->retired=true;current_.reset();Impl::collect();}
Offer Producer::connect(const Peer& consumer){
 if(GetCurrentThreadId()!=config_->thread)throw Error(Status::ProtocolError,E_UNEXPECTED,"producer owner thread");
 auto peer=verified(consumer);
 if(retire()==Status::PeerBusy)throw Error(Status::PeerBusy,E_PENDING,"previous importer still owns bundle");
 std::lock_guard lock(Impl::mutex());Impl::collect();auto& vault=Impl::vault();
 const auto available=std::find(vault.begin(),vault.end(),nullptr);
 if(available==vault.end())throw Error(Status::RetirementCapacity,E_PENDING,"two unresolved physical generations");
 auto p=std::make_shared<Impl>();p->peer=std::move(peer);p->device=config_->device;p->graphics=config_->graphics;
 static uint64_t lastGeneration=0;
 if(lastGeneration==UINT64_MAX)throw Error(Status::SequenceExhausted,E_FAIL,"generation exhausted");
 const auto nextGeneration=++lastGeneration;p->offer={config_->name+L"."+std::to_wstring(nextGeneration),nextGeneration};
 p->mapping.handle=Handle(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(Header)),p->offer.mapping.c_str()));
 const auto mappingError=GetLastError();win(p->mapping.handle.value!=nullptr,"CreateFileMapping");
 if(mappingError==ERROR_ALREADY_EXISTS)throw Error(Status::ProtocolError,HRESULT_FROM_WIN32(mappingError),"mapping name already exists");
 p->mapping.map();auto& h=*p->mapping.header;
 h.magic=magic;h.version=protocol_version;h.bytes=sizeof(Header);
 h.generation=nextGeneration;
 h.luid=config_->adapter;const auto self=identity(GetCurrentProcess());
 h.producerPid=self.pid;h.producerCreation=self.creation;h.consumerPid=consumer.pid;h.consumerCreation=consumer.creation;
 auto duplicate=[&](HANDLE source,size_t index){
  win(DuplicateHandle(GetCurrentProcess(),source,p->peer.value,&p->remote[index],0,FALSE,DUPLICATE_SAME_ACCESS)!=FALSE,"DuplicateHandle offer");
  h.handles[index]=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p->remote[index]));
 };
 for(size_t i=0;i<p->slots.size();++i){
  auto& slot=p->slots[i];D3D11_TEXTURE2D_DESC d{};d.Width=config_->shape.width;d.Height=config_->shape.height*(config_->shape.splitCompare?2u:1u);
  d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=config_->shape.format;d.Usage=D3D11_USAGE_DEFAULT;
  d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  check(p->device->CreateTexture2D(&d,nullptr,&slot.texture),"shared texture");Ptr<IDXGIResource1> dxgi;
  check(slot.texture.As(&dxgi),"shared DXGI texture");Handle texture;
  check(dxgi->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&texture.value),"texture NT handle");duplicate(texture.value,i*3);
  for(size_t f=0;f<2;++f){
   auto& fence=f==0?slot.write:slot.returned;check(p->device->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"shared fence");
   Handle handle;check(fence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle.value),"fence NT handle");duplicate(handle.value,i*3+f+1);
  }
 }
 p->frameEvent=Handle(CreateEventW(nullptr,FALSE,FALSE,nullptr));win(p->frameEvent.value!=nullptr,"frame event");
 p->controlEvent=Handle(CreateEventW(nullptr,FALSE,FALSE,nullptr));win(p->controlEvent.value!=nullptr,"control event");
 duplicate(p->frameEvent.value,12);duplicate(p->controlEvent.value,13);
 store(h.transfer,static_cast<uint64_t>(Transfer::Offered));*available=p;current_=p;return p->offer;
}
Status Producer::status()const{
 if(!current_)return Status::Closed;if(current_->dead())return Status::PeerDead;
 if(current_->device->GetDeviceRemovedReason()!=S_OK)return Status::DeviceLost;
 if(current_->uncertain)return Status::ProtocolError;
 switch(transfer(*current_->mapping.header)){
 case Transfer::Initial:case Transfer::Offered:case Transfer::Importing:return Status::Pending;
 case Transfer::Ready:return Status::Ok;case Transfer::Rejected:return Status::Rejected;
 case Transfer::Closed:case Transfer::Revoked:return Status::Closed;
 }
 return Status::ProtocolError;
}
Status Producer::publish(ID3D11Texture2D* source,uint64_t timestamp,uint64_t arrival,bool discontinuity,uint64_t obsTick,
 ID3D11Texture2D* before,uint32_t splitPercent){
 if(GetCurrentThreadId()!=config_->thread)return Status::ProtocolError;
 const auto state=status();if(state!=Status::Ok)return state;if(!source)return Status::InvalidFrame;
 D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);Ptr<ID3D11Device> owner;source->GetDevice(&owner);
 if(owner.Get()!=config_->device.Get()||d.Width!=config_->shape.width||d.Height!=config_->shape.height||
  !compatible(d.Format,config_->shape.format)||d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1)return Status::InvalidFrame;
 if(config_->shape.splitCompare){
  if(!before||splitPercent>100)return Status::InvalidFrame;
  D3D11_TEXTURE2D_DESC original{};before->GetDesc(&original);Ptr<ID3D11Device> originalOwner;before->GetDevice(&originalOwner);
  if(originalOwner.Get()!=config_->device.Get()||original.Width!=d.Width||original.Height!=d.Height||
   !compatible(original.Format,config_->shape.format)||original.MipLevels!=1||original.ArraySize!=1||original.SampleDesc.Count!=1)return Status::InvalidFrame;
 }
 auto& p=*current_;size_t index=0;
 for(;index<p.slots.size();++index){
  const auto& slot=p.slots[index];const auto write=slot.write->GetCompletedValue(),returned=slot.returned->GetCompletedValue();
  if(write==UINT64_MAX||returned==UINT64_MAX)return Status::DeviceLost;
  if(complete(write,slot.target)&&complete(returned,slot.target))break;
 }
 if(index==p.slots.size())return Status::Dropped;
 const uint64_t step=discontinuity && p.serial ? 2 : 1;
 if(p.serial>max_serial-step)return Status::SequenceExhausted;
 auto& slot=p.slots[index];p.serial+=step;slot.target=p.serial;p.uncertain=true;
 if(config_->shape.splitCompare){
  p.graphics->CopySubresourceRegion(slot.texture.Get(),0,0,0,0,source,0,nullptr);
  p.graphics->CopySubresourceRegion(slot.texture.Get(),0,0,config_->shape.height,0,before,0,nullptr);
 }else p.graphics->CopyResource(slot.texture.Get(),source);
 if(FAILED(p.graphics->Signal(slot.write.Get(),slot.target)))return Status::ProtocolError;
 p.graphics->Flush();p.uncertain=false;
 auto& wire=p.mapping.header->frames[index];const auto& shape=config_->shape;
 std::array<uint64_t,16> words{generation(),slot.target,timestamp,arrival,(uint64_t{shape.height}<<32)|shape.width,
  (uint64_t{static_cast<uint32_t>(shape.colorSpace)}<<32)|static_cast<uint32_t>(shape.format),
  (shape.displayCapture?uint64_t{1}:uint64_t{0})|(shape.splitCompare?(uint64_t{2}|(uint64_t{splitPercent}<<8)):uint64_t{0})};
 std::memcpy(words.data()+7,shape.sourceMonitor.data(),sizeof(shape.sourceMonitor));
 words[15]=obsTick;
 store(wire.sequence,slot.target*2-1);
 for(size_t i=0;i<words.size();++i)store(wire.words[i],words[i]);
 store(wire.sequence,slot.target*2);
 if(!SetEvent(p.frameEvent.value))return Status::ProtocolError;
 return Status::Ok;
}
Status Producer::retire(){
 if(GetCurrentThreadId()!=config_->thread)return Status::ProtocolError;
 if(!current_)return Status::Closed;std::lock_guard lock(Impl::mutex());
 if(!current_->dead()){
  switch(transfer(*current_->mapping.header)){
  case Transfer::Closed:case Transfer::Rejected:break;
  case Transfer::Initial:case Transfer::Offered:case Transfer::Revoked:if(!current_->revoke())return Status::PeerBusy;break;
  case Transfer::Importing:case Transfer::Ready:return Status::PeerBusy;
  }
 }
 const auto error=current_->revokeError;
 current_->retired=true;current_.reset();Impl::collect();
 return error==ERROR_SUCCESS?Status::Ok:Status::ProtocolError;
}
HANDLE Producer::peer_process()const{return current_?current_->peer.value:nullptr;}
HANDLE Producer::control_event()const{return current_?current_->controlEvent.value:nullptr;}
uint64_t Producer::generation()const{return current_?current_->offer.generation:0;}
size_t Producer::retained_generations(){std::lock_guard lock(Impl::mutex());Impl::collect();return static_cast<size_t>(std::count_if(Impl::vault().begin(),Impl::vault().end(),[](const auto& p){return p!=nullptr;}));}
bool Producer::generation_retained(uint64_t generation){
 std::lock_guard lock(Impl::mutex());Impl::collect();
 return std::any_of(Impl::vault().begin(),Impl::vault().end(),[generation](const auto& p){return p&&p->offer.generation==generation;});
}

struct Consumer::Impl {
 struct Slot {
  Ptr<ID3D12Resource> texture,destination,before;Ptr<ID3D12Fence> write,returned;
  Ptr<ID3D12CommandAllocator> allocator;Ptr<ID3D12GraphicsCommandList> list;
  uint64_t submitted=0;
 };
 Mapping mapping;Handle peer,frameEvent,controlEvent;Ptr<ID3D12Device> device;Ptr<ID3D12CommandQueue> queue;
 std::array<Slot,4> slots;uint64_t generation=0;DWORD thread=GetCurrentThreadId();
 detail::CopySequence sequence;
 bool retired=false,uncertain=false,closed=false;
#ifdef NR_FG_IPC_TEST_PROBE
 HANDLE reached=nullptr,resume=nullptr;
#endif
 static auto& vault(){static std::array<std::shared_ptr<Impl>,generation_limit> entries;return entries;}
 static auto& mutex(){static std::mutex m;return m;}
 bool drain(){
  if(closed)return true;if(uncertain)return false;
  for(const auto& slot:slots)if(slot.submitted&&!complete(slot.returned->GetCompletedValue(),slot.submitted))return false;
  for(auto& slot:slots){slot.list.Reset();slot.allocator.Reset();slot.destination.Reset();slot.before.Reset();slot.texture.Reset();slot.write.Reset();slot.returned.Reset();}
  store(mapping.header->transfer,static_cast<uint64_t>(Transfer::Closed));SetEvent(controlEvent.value);closed=true;return true;
 }
 static void collect(){for(auto& p:vault())if(p&&p->retired&&p->drain())p.reset();}
};
Consumer::Consumer(ID3D12CommandQueue* queue,const Offer& offer,const Peer& producer){
 if(!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw Error(Status::ProtocolError,E_INVALIDARG,"consumer direct queue");
 std::lock_guard lock(Impl::mutex());Impl::collect();auto& vault=Impl::vault();
 const auto available=std::find(vault.begin(),vault.end(),nullptr);
 if(available==vault.end())throw Error(Status::RetirementCapacity,E_PENDING,"consumer retirement capacity");
 auto p=std::make_shared<Impl>();p->peer=verified(producer);p->queue=queue;
 check(queue->GetDevice(IID_PPV_ARGS(&p->device)),"consumer device");
 p->mapping.handle=Handle(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,offer.mapping.c_str()));win(p->mapping.handle.value!=nullptr,"OpenFileMapping");p->mapping.map();
 auto& h=*p->mapping.header;const auto validity=validate(h,p->device->GetAdapterLuid());
 if(validity!=Status::Ok)throw Error(validity,E_INVALIDARG,"IPC version/adapter");
 const auto self=identity(GetCurrentProcess());
 if(h.generation!=offer.generation||h.producerPid!=producer.pid||h.producerCreation!=producer.creation||
  h.consumerPid!=self.pid||h.consumerCreation!=self.creation)throw Error(Status::WrongPeer,E_INVALIDARG,"IPC offer identity");
 if(!exchange(h,Transfer::Offered,Transfer::Importing))throw Error(Status::PeerBusy,E_PENDING,"offer already claimed/revoked");
 std::array<Handle,14> incoming;
 for(size_t i=0;i<incoming.size();++i)incoming[i]=Handle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h.handles[i])));
 p->frameEvent=std::move(incoming[12]);p->controlEvent=std::move(incoming[13]);p->generation=h.generation;
 try{
  for(size_t i=0;i<p->slots.size();++i){
   auto& slot=p->slots[i];check(p->device->OpenSharedHandle(incoming[i*3].value,IID_PPV_ARGS(&slot.texture)),"import texture");
   check(p->device->OpenSharedHandle(incoming[i*3+1].value,IID_PPV_ARGS(&slot.write)),"import write fence");
   check(p->device->OpenSharedHandle(incoming[i*3+2].value,IID_PPV_ARGS(&slot.returned)),"import return fence");
   check(p->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocator)),"copy allocator");
   check(p->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(&slot.list)),"copy list");
   check(slot.list->Close(),"initial copy close");
  }
 }catch(const Error&){
  for(auto& slot:p->slots)slot={};
  for(auto& handle:incoming)handle=Handle{};
  store(h.transfer,static_cast<uint64_t>(Transfer::Rejected));SetEvent(p->controlEvent.value);throw;
 }
 for(auto& handle:incoming)handle=Handle{};
 store(h.transfer,static_cast<uint64_t>(Transfer::Ready));win(SetEvent(p->controlEvent.value)!=FALSE,"import ack");
 *available=p;impl_=std::move(p);
}
Consumer::~Consumer(){std::lock_guard lock(Impl::mutex());if(impl_)impl_->retired=true;impl_.reset();Impl::collect();}
HANDLE Consumer::frame_event()const{return impl_?impl_->frameEvent.value:nullptr;}
HANDLE Consumer::producer_process()const{return impl_?impl_->peer.value:nullptr;}
Status Consumer::status()const{
 if(!impl_||impl_->closed)return Status::Closed;
 if(WaitForSingleObject(impl_->peer.value,0)==WAIT_OBJECT_0)return Status::PeerDead;
 if(impl_->device->GetDeviceRemovedReason()!=S_OK)return Status::DeviceLost;
 return impl_->uncertain?Status::ProtocolError:Status::Ok;
}
std::optional<Frame> Consumer::frame(size_t index)const{
 if(status()!=Status::Ok||index>=impl_->slots.size())return {};
 auto f=snapshot(impl_->mapping.header->frames[index]);
 return f&&f->generation==impl_->generation&&f->serial>impl_->slots[index].submitted?f:std::nullopt;
}
Status Consumer::arm_return(size_t index,HANDLE event){
 if(!event)return Status::InvalidFrame; // A null D3D12 completion event would CPU-block.
 const auto f=frame(index);if(!f)return Status::Pending;
 return SUCCEEDED(impl_->slots[index].returned->SetEventOnCompletion(f->serial,event))?Status::Ok:Status::ProtocolError;
}
Status Consumer::arm_return(const CopyTicket& ticket,HANDLE event){
 if(!event)return Status::InvalidFrame;
 const auto status=copy_status(ticket);
 if(status!=Status::Pending&&status!=Status::Ok)return status;
 return SUCCEEDED(impl_->slots[ticket.slot].returned->SetEventOnCompletion(ticket.frame.serial,event))?Status::Ok:Status::ProtocolError;
}
Status Consumer::copy(size_t index,ID3D12Resource* destination){
 if(!impl_||GetCurrentThreadId()!=impl_->thread)return Status::ProtocolError;
 impl_->sequence.break_pair();
 const auto f=frame(index);if(!f)return Status::Pending;
 if(f->shape.splitCompare)return Status::InvalidFrame;
 return copy_frame({index,*f},destination,nullptr,false);
}
CopyStep Consumer::copy_next(ID3D12Resource* destination,ID3D12Resource* before){
 if(!impl_)return {Status::Closed};
 if(GetCurrentThreadId()!=impl_->thread)return {Status::ProtocolError};
 const auto state=status();auto& sequence=impl_->sequence;
 if(state!=Status::Ok){sequence.break_pair();return {state};}
 detail::CopySequence::Slots visible{};
 for(size_t i=0;i<visible.size();++i)visible[i]=frame(i);
 const auto ticket=detail::CopySequence::oldest(visible);
 if(!ticket)return {Status::Pending};
 const auto& previous=sequence.previous();
 if(!destination||destination->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
    (ticket->frame.shape.splitCompare&&(!before||before==destination))||
    (previous&&(impl_->slots[previous->slot].destination.Get()==destination||
     impl_->slots[previous->slot].before.Get()==destination||
     (ticket->frame.shape.splitCompare&&(impl_->slots[previous->slot].destination.Get()==before||
      impl_->slots[previous->slot].before.Get()==before))))){
  return sequence.record(*ticket,Status::InvalidFrame);
 }
 // This exact serial still owns the source slot until its return fence signals.
 return sequence.record(*ticket,copy_frame(*ticket,destination,ticket->frame.shape.splitCompare?before:nullptr,true));
}
Status Consumer::copy_status(const CopyTicket& ticket)const{
 if(!impl_)return Status::Closed;
 if(GetCurrentThreadId()!=impl_->thread)return Status::ProtocolError;
 const auto state=status();if(state!=Status::Ok)return state;
 if(ticket.slot>=impl_->slots.size()||ticket.frame.generation!=impl_->generation||!ticket.frame.serial)return Status::InvalidFrame;
 const auto& slot=impl_->slots[ticket.slot];
 if(ticket.frame.serial>slot.submitted)return Status::InvalidFrame;
 return detail::copy_completion(slot.returned->GetCompletedValue(),ticket.frame.serial);
}
Status Consumer::order_copy(const CopyTicket& ticket,ID3D12CommandQueue* reader)const{
 if(!reader)return Status::InvalidFrame;
 const auto state=copy_status(ticket);
 if(state!=Status::Pending)return state;
 const auto& slot=impl_->slots[ticket.slot];
 return SUCCEEDED(reader->Wait(slot.returned.Get(),ticket.frame.serial))?Status::Ok:Status::ProtocolError;
}
Status Consumer::copy_frame(const CopyTicket& ticket,ID3D12Resource* destination,ID3D12Resource* before,bool guardCrossSlotDestination){
 if(!destination)return Status::InvalidFrame;
 const auto state=status();if(state!=Status::Ok)return state;
 const auto* f=&ticket.frame;
 auto& p=*impl_;auto& slot=p.slots[ticket.slot];
 for(const auto& other:p.slots){
  if(other.submitted&&(&other==&slot||(guardCrossSlotDestination&&
   (other.destination.Get()==destination||other.before.Get()==destination||
    (before&&(other.destination.Get()==before||other.before.Get()==before)))))){
   const auto ready=detail::copy_completion(other.returned->GetCompletedValue(),other.submitted);
   if(ready!=Status::Ok)return ready;
  }
 }
 const auto source=slot.texture->GetDesc(),target=destination->GetDesc();Ptr<ID3D12Device> owner;
 if(FAILED(destination->GetDevice(IID_PPV_ARGS(&owner)))||owner.Get()!=p.device.Get())return Status::InvalidFrame;
 if(source.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||source.Width!=f->shape.width||
  source.Height!=uint64_t{f->shape.height}*(f->shape.splitCompare?2u:1u)||source.Format!=f->shape.format||
  source.MipLevels!=1||source.DepthOrArraySize!=1||source.SampleDesc.Count!=1)return Status::InvalidFrame;
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 bytes=0;p.device->GetCopyableFootprints(&source,0,1,0,&layout,nullptr,nullptr,&bytes);
 const bool readback=target.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;
 if(readback){D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
  if(FAILED(destination->GetHeapProperties(&heap,&flags))||heap.Type!=D3D12_HEAP_TYPE_READBACK||target.Width<bytes)return Status::InvalidFrame;
 }else if(target.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||target.Width!=f->shape.width||target.Height!=f->shape.height||
  target.Format!=source.Format||target.MipLevels!=1||target.DepthOrArraySize!=1||target.SampleDesc.Count!=1)return Status::InvalidFrame;
 if(f->shape.splitCompare){
  if(readback||!before||before==destination)return Status::InvalidFrame;
  Ptr<ID3D12Device> beforeOwner;const auto original=before->GetDesc();
  if(FAILED(before->GetDevice(IID_PPV_ARGS(&beforeOwner)))||beforeOwner.Get()!=p.device.Get()||
   original.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||original.Width!=target.Width||original.Height!=target.Height||
   original.Format!=target.Format||original.MipLevels!=1||original.DepthOrArraySize!=1||original.SampleDesc.Count!=1)return Status::InvalidFrame;
 }
 if(FAILED(slot.allocator->Reset())||FAILED(slot.list->Reset(slot.allocator.Get(),nullptr)))return Status::ProtocolError;
 transition(slot.list.Get(),slot.texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
 if(readback){
  D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=slot.texture.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=destination;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=layout;
  slot.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
 }else if(f->shape.splitCompare){
  transition(slot.list.Get(),destination,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
  transition(slot.list.Get(),before,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=slot.texture.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=destination;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_BOX box{0,0,0,f->shape.width,f->shape.height,1};
  slot.list->CopyTextureRegion(&to,0,0,0,&from,&box);
  box.top=f->shape.height;box.bottom=f->shape.height*2;to.pResource=before;
  slot.list->CopyTextureRegion(&to,0,0,0,&from,&box);
  transition(slot.list.Get(),before,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
  transition(slot.list.Get(),destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
 }else{
  transition(slot.list.Get(),destination,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
  slot.list->CopyResource(destination,slot.texture.Get());transition(slot.list.Get(),destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
 }
 transition(slot.list.Get(),slot.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
 if(FAILED(slot.list->Close()))return Status::ProtocolError;
 if(FAILED(p.queue->Wait(slot.write.Get(),f->serial)))return Status::ProtocolError;
 // Reserve before Execute: a crash or Signal failure must leave a known pending
 // target in both processes. No CPU receipt after submission can prove safety.
 slot.destination=destination;slot.before=before;slot.submitted=f->serial;p.uncertain=true;
 ID3D12CommandList* lists[]{slot.list.Get()};p.queue->ExecuteCommandLists(1,lists);
#ifdef NR_FG_IPC_TEST_PROBE
 if(p.reached){
  win(SetEvent(p.reached)!=FALSE,"test submission checkpoint");
  if(WaitForSingleObject(p.resume,10000)!=WAIT_OBJECT_0)throw Error(Status::ProtocolError,E_PENDING,"test submission checkpoint timeout");
 }
#endif
 if(FAILED(p.queue->Signal(slot.returned.Get(),f->serial)))return Status::ProtocolError;
 p.uncertain=false;return Status::Ok;
}
Status Consumer::close(){
 if(!impl_)return Status::Closed;
 if(GetCurrentThreadId()!=impl_->thread)return Status::ProtocolError;
 return impl_->drain()?Status::Closed:Status::Pending;
}
#ifdef NR_FG_IPC_TEST_PROBE
void Consumer::test_submit_probe(HANDLE reached,HANDLE resume){impl_->reached=reached;impl_->resume=resume;}
#endif
} // namespace nr::fg::ipc
