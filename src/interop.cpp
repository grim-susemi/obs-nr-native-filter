// SPDX-License-Identifier: GPL-3.0-only
#include "interop.h"
#include <atomic>
#include <array>
#include <stdexcept>
namespace nr::interop {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("interop HRESULT failure: "+std::to_string(static_cast<uint32_t>(hr)));}
struct Handle {
 HANDLE value=nullptr;
 ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
 D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
}
}
bool same_identity(IUnknown* a,IUnknown* b){
 if(!a||!b)return false;Ptr<IUnknown> x,y;
 return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x.Get()==y.Get();
}
LUID renderer_luid(ID3D11Device* d){
 if(!d)throw std::runtime_error("renderer missing");Ptr<IDXGIDevice> dxgi;Ptr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
 check(d->QueryInterface(IID_PPV_ARGS(&dxgi)));check(dxgi->GetAdapter(&adapter));check(adapter->GetDesc(&desc));return desc.AdapterLuid;
}
bool compatible_context(ID3D11Device* renderer,ID3D12Device* device,ID3D12CommandQueue* queue){
 if(!renderer||!device||!queue||!same_luid(renderer_luid(renderer),device->GetAdapterLuid()))return false;
 Ptr<ID3D12Device> owned;return SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&owned)))&&same_identity(owned.Get(),device);
}
HRESULT open_shared_resource(ID3D12Device* device,HANDLE handle,ID3D12Resource** out){
 if(!device||!out)return E_INVALIDARG;*out=nullptr;return device->OpenSharedHandle(handle,IID_PPV_ARGS(out));
}
Context::Context(ID3D11Device* renderer):graphicsThread_(GetCurrentThreadId()),luid_(renderer_luid(renderer)){
 check(renderer->QueryInterface(IID_PPV_ARGS(&renderer_)));Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter1> adapter;
 check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));check(factory->EnumAdapterByLuid(luid_,IID_PPV_ARGS(&adapter)));
 check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device_)));
 D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
 check(device_->CreateCommandQueue(&q,IID_PPV_ARGS(&queue_)));
 if(!compatible_context(renderer,device_.Get(),queue_.Get()))throw std::runtime_error("interop adapter/canonical queue identity mismatch");
}
std::shared_ptr<Context> Context::create(ID3D11Device* d){return std::shared_ptr<Context>(new Context(d));}
struct Bridge::Impl {
 struct Slot {
  std::atomic<std::shared_ptr<const JobKey>> key;
  std::atomic<WorkPhase> phase{WorkPhase::Empty};
  std::atomic<uint64_t> producer{0},worker{0},consumer{0};
  std::atomic<uint64_t> consumerLease{0};
  std::atomic<bool> consumerOpen{false},cancelled{false},failed{false};
  bool consumerRaw=false; // Graphics owner only.
  std::shared_ptr<ConsumerEvent> endEvent;
  std::shared_ptr<const ConsumerEnd> endReceipt;
  Ptr<ID3D11Texture2D> raw,input11,output11;
  Ptr<ID3D12Resource> input12,output12;
  Ptr<ID3D12CommandAllocator> allocator;
  Ptr<ID3D12GraphicsCommandList> commands;
 };
 std::shared_ptr<Context> context;
 const uint64_t generation;
 const FrameDescriptor descriptor;
 Ptr<ID3D11Device5> device11;
 Ptr<ID3D11DeviceContext4> graphics;
 Ptr<ID3D11Fence> producer11,worker11,consumer11;
 Ptr<ID3D12Fence> producer12,worker12;
 Ptr<ID3DDeviceContextState> blitState;
 Ptr<ID3D11VertexShader> vertex;
 Ptr<ID3D11PixelShader> pixel;
 Ptr<ID3D11RasterizerState> raster;
 std::array<Slot,3> slots;
 FenceCursor producerCursor,workerCursor,consumerCursor,leaseCursor;
 uint64_t lastSubmission=0;
 std::atomic<DWORD> workerThread{0};
 Impl(std::shared_ptr<Context> c,uint64_t g,const FrameDescriptor& d,ID3D11Device5* renderer,
      std::span<const std::byte> vs,std::span<const std::byte> ps):context(std::move(c)),generation(g),descriptor(d),device11(renderer){
  if(!generation||!descriptor.valid()||vs.empty()||ps.empty())throw std::runtime_error("invalid interop generation/descriptor/shader");
  Ptr<ID3D11DeviceContext> immediate;device11->GetImmediateContext(&immediate);check(immediate.As(&graphics));
  D3D_FEATURE_LEVEL selected{};const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
  check(device11->CreateDeviceContextState(device11->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED,
   levels,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&selected,&blitState));
  check(device11->CreateVertexShader(vs.data(),vs.size(),nullptr,&vertex));
  check(device11->CreatePixelShader(ps.data(),ps.size(),nullptr,&pixel));
  D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
  check(device11->CreateRasterizerState(&rd,&raster));
  check(device11->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&producer11)));
  check(device11->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&consumer11)));
  {Handle h;check(producer11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&h.value));
   check(context->device()->OpenSharedHandle(h.value,IID_PPV_ARGS(&producer12)));}
  check(context->device()->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&worker12)));
  {Handle h;check(context->device()->CreateSharedHandle(worker12.Get(),nullptr,GENERIC_ALL,nullptr,&h.value));
   check(device11->OpenSharedFence(h.value,IID_PPV_ARGS(&worker11)));}
  for(auto& s:slots){
   D3D11_TEXTURE2D_DESC td{};td.Width=descriptor.allocation.width;td.Height=descriptor.allocation.height;
   td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=source_format(descriptor.format);
   td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
   check(device11->CreateTexture2D(&td,nullptr,&s.raw));
   td.Format=canonical_format(descriptor.format);td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
   td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
   auto share=[&](Ptr<ID3D11Texture2D>& texture,Ptr<ID3D12Resource>& resource){
    check(device11->CreateTexture2D(&td,nullptr,&texture));Ptr<IDXGIResource1> dxgi;check(texture.As(&dxgi));Handle h;
    check(dxgi->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&h.value));
    check(open_shared_resource(context->device(),h.value,&resource));
   };share(s.input11,s.input12);share(s.output11,s.output12);
   check(context->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator)));
   check(context->device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.commands)));
   check(s.commands->Close());
  }
 }
 bool on_graphics()const{return GetCurrentThreadId()==context->graphics_thread();}
 bool on_worker(){DWORD current=GetCurrentThreadId(),empty=0;workerThread.compare_exchange_strong(empty,current);return workerThread.load()==current;}
 Slot* find(const Ticket& t){if(t.slot>=slots.size()||t.key.resourceGeneration!=generation)return nullptr;
  auto& s=slots[t.slot];const auto k=s.key.load();return k&&*k==t.key?&s:nullptr;}
 Ownership snapshot(size_t index)const{
  if(index>=slots.size())return {std::nullopt,WorkPhase::Captured,0,0,0,0,0,0,false,false,true};
  const auto& s=slots[index];const auto phase=s.phase.load();const auto key=s.key.load();
  return {key?std::optional<JobKey>{*key}:std::nullopt,phase,s.producer.load(),s.worker.load(),s.consumer.load(),
   producer11->GetCompletedValue(),worker12->GetCompletedValue(),consumer11->GetCompletedValue(),
   s.consumerOpen.load(),s.cancelled.load(),s.failed.load(),s.consumerLease.load()};
 }
 bool shape(ID3D11Texture2D* texture,DXGI_FORMAT format,bool borrowed=false)const{
  if(!texture)return false;Ptr<ID3D11Device> device;texture->GetDevice(&device);
  if(!same_identity(device.Get(),device11.Get()))return false;D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
  const bool compatible=borrowed?source_format_compatible(d.Format,descriptor.format):d.Format==format;
  return d.Width==descriptor.allocation.width&&d.Height==descriptor.allocation.height&&compatible&&
   d.MipLevels==1&&d.ArraySize==1&&d.SampleDesc.Count==1;
 }
 void convert(ID3D11Texture2D* from,ID3D11Texture2D* to){
  D3D11_TEXTURE2D_DESC a{},b{};from->GetDesc(&a);to->GetDesc(&b);
  if(a.Format==b.Format){graphics->CopyResource(to,from);return;}
  Ptr<ID3D11ShaderResourceView> srv;Ptr<ID3D11RenderTargetView> rtv;
  check(device11->CreateShaderResourceView(from,nullptr,&srv));check(device11->CreateRenderTargetView(to,nullptr,&rtv));
  struct Restore{ID3D11DeviceContext4* graphics;Ptr<ID3DDeviceContextState> saved;
   ~Restore(){if(saved)graphics->SwapDeviceContextState(saved.Get(),nullptr);}}restore{graphics.Get(),{}};
  graphics->SwapDeviceContextState(blitState.Get(),&restore.saved);
  ID3D11RenderTargetView* target=rtv.Get();graphics->OMSetRenderTargets(1,&target,nullptr);
  graphics->OMSetBlendState(nullptr,nullptr,UINT_MAX);graphics->OMSetDepthStencilState(nullptr,0);
  graphics->RSSetState(raster.Get());D3D11_VIEWPORT vp{0,0,static_cast<float>(b.Width),static_cast<float>(b.Height),0,1};graphics->RSSetViewports(1,&vp);
  graphics->IASetInputLayout(nullptr);graphics->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  graphics->VSSetShader(vertex.Get(),nullptr,0);graphics->PSSetShader(pixel.Get(),nullptr,0);
  graphics->GSSetShader(nullptr,nullptr,0);graphics->HSSetShader(nullptr,nullptr,0);graphics->DSSetShader(nullptr,nullptr,0);
  ID3D11ShaderResourceView* image=srv.Get();graphics->PSSetShaderResources(0,1,&image);graphics->Draw(3,0);
 }
};
Bridge::Bridge(std::shared_ptr<Context> c,uint64_t g,const FrameDescriptor& d,std::span<const std::byte> vs,std::span<const std::byte> ps){
 if(!c||GetCurrentThreadId()!=c->graphics_thread())throw std::runtime_error("interop graphics owner required");
 impl_=std::make_unique<Impl>(c,g,d,c->renderer_.Get(),vs,ps);
}
Bridge::~Bridge(){if(impl_&&!quiescent())(void)impl_.release();} // Deliberately retain unresolved physical ownership, never wait/free.
std::optional<Ticket> Bridge::capture(const JobKey& key,ID3D11Texture2D* borrowed){
 if(!impl_||!impl_->on_graphics()||!key.valid()||key.resourceGeneration!=impl_->generation||
    key.submissionSerial<=impl_->lastSubmission||!impl_->shape(borrowed,source_format(impl_->descriptor.format),true))return std::nullopt;
 size_t index=0;for(;index<3;++index)if(reusable(impl_->snapshot(index)))break;
 return capture_at(index,key,borrowed);
}
std::optional<Ticket> Bridge::capture_at(size_t index,const JobKey& key,ID3D11Texture2D* borrowed){
 if(!impl_||!impl_->on_graphics()||index>=impl_->slots.size()||!key.valid()||key.resourceGeneration!=impl_->generation||
    key.submissionSerial<=impl_->lastSubmission||!impl_->shape(borrowed,source_format(impl_->descriptor.format),true)||
    !reusable(impl_->snapshot(index)))return std::nullopt;
 auto& s=impl_->slots[index];const auto value=impl_->producerCursor.reserve();if(!value)return std::nullopt;
 s.producer=value;s.worker=0;s.consumer=0;s.consumerLease=0;s.consumerOpen=false;s.cancelled=false;s.failed=false;
 s.endEvent.reset();s.endReceipt.reset();
 s.key.store(std::make_shared<const JobKey>(key));s.phase=WorkPhase::Captured;impl_->lastSubmission=key.submissionSerial;
 try{impl_->graphics->CopyResource(s.raw.Get(),borrowed);impl_->convert(s.raw.Get(),s.input11.Get());
  check(impl_->graphics->Signal(impl_->producer11.Get(),value));impl_->graphics->Flush();
 }catch(const std::exception&){s.failed=true;return std::nullopt;}
 return Ticket{key,index};
}
std::optional<WorkerView> Bridge::begin_worker(const Ticket& t){
 if(!impl_||!impl_->on_worker())return std::nullopt;auto* s=impl_->find(t);if(!s||s->failed)return std::nullopt;
 auto expected=WorkPhase::Captured;if(!s->phase.compare_exchange_strong(expected,WorkPhase::Recording))return std::nullopt;
 if(impl_->find(t)!=s){s->phase=WorkPhase::Captured;return std::nullopt;}
 const auto value=impl_->workerCursor.reserve();if(!value){s->failed=true;return std::nullopt;}s->worker=value;
 try{check(impl_->context->queue()->Wait(impl_->producer12.Get(),s->producer));
  check(s->allocator->Reset());check(s->commands->Reset(s->allocator.Get(),nullptr));
 }catch(const std::exception&){s->failed=true;return std::nullopt;}
 return WorkerView{s->commands.Get(),s->input12.Get(),s->output12.Get(),value};
}
bool Bridge::submit_worker(const Ticket& t){
 if(!impl_||!impl_->on_worker())return false;auto* s=impl_->find(t);
 if(!s||s->failed||s->phase!=WorkPhase::Recording)return false;
 if(FAILED(s->commands->Close())){s->failed=true;return false;}
 ID3D12CommandList* lists[]{s->commands.Get()};impl_->context->queue()->ExecuteCommandLists(1,lists);
 if(FAILED(impl_->context->queue()->Signal(impl_->worker12.Get(),s->worker))){s->failed=true;return false;}
 s->phase=WorkPhase::Submitted;return true;
}
bool Bridge::record_copy(const Ticket& t){
 const auto v=begin_worker(t);if(!v)return false;
 barrier(v->commands,v->input,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
 barrier(v->commands,v->output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
 v->commands->CopyResource(v->output,v->input);
 barrier(v->commands,v->input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
 barrier(v->commands,v->output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
 return submit_worker(t);
}
bool Bridge::discard_recording(const Ticket& t){
 if(!impl_||!impl_->on_worker())return false;auto* s=impl_->find(t);if(!s||s->failed)return false;
 auto phase=s->phase.load();if(phase!=WorkPhase::Captured&&phase!=WorkPhase::Recording)return false;
 if(!s->phase.compare_exchange_strong(phase,WorkPhase::Recording))return false;
 if(impl_->find(t)!=s){s->phase=phase;return false;}
 if(phase==WorkPhase::Recording&&FAILED(s->commands->Close())){s->failed=true;return false;}
 s->worker=0;s->phase=WorkPhase::Discarded;return true;
}
std::optional<Consumer> Bridge::acquire_raw(const Ticket& t){
 if(!impl_||!impl_->on_graphics())return std::nullopt;auto* s=impl_->find(t);
 if(!s||s->failed||s->consumerOpen||impl_->producer11->GetCompletedValue()==UINT64_MAX||
    impl_->consumer11->GetCompletedValue()==UINT64_MAX)return std::nullopt;
 const auto value=impl_->leaseCursor.reserve();if(!value)return std::nullopt;
 s->consumerLease=value;s->consumerRaw=true;s->consumerOpen=true;
 return Consumer{t,s->raw.Get(),value,true,this,impl_->consumer11,impl_->context->graphics_thread()};
}
std::optional<Consumer> Bridge::acquire_output(const Ticket& t){
 if(!impl_||!impl_->on_graphics())return std::nullopt;auto* s=impl_->find(t);
 if(!s||s->consumerOpen||!publishable(impl_->snapshot(t.slot),t.key))return std::nullopt;
 const auto value=impl_->leaseCursor.reserve();if(!value)return std::nullopt;
 s->consumerLease=value;s->consumerRaw=false;s->consumerOpen=true;
 return Consumer{t,s->output11.Get(),value,false,this,impl_->consumer11,impl_->context->graphics_thread()};
}
bool Bridge::owns_consumer(const Consumer& c)const{
 if(!impl_||!impl_->on_graphics()||c.bridge!=this||c.fence.Get()!=impl_->consumer11.Get()||
    c.graphicsOwner!=impl_->context->graphics_thread())return false;auto* s=impl_->find(c.ticket);
 return s&&s->consumerOpen&&!s->failed&&c.leaseValue&&s->consumerLease==c.leaseValue&&
    s->consumerRaw==c.raw&&c.texture==(c.raw?s->raw.Get():s->output11.Get());
}
bool Bridge::copy_to(const Consumer& c,ID3D11Texture2D* target){
 if(!owns_consumer(c)||!impl_->shape(target,source_format(impl_->descriptor.format)))return false;
 auto* s=impl_->find(c.ticket);
 try{impl_->convert(c.texture,target);}catch(const std::exception&){s->failed=true;return false;}return true;
}
std::shared_ptr<ConsumerEvent> ConsumerEvent::create(){
 const auto handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 return handle?std::shared_ptr<ConsumerEvent>(new ConsumerEvent(handle)):nullptr;
}
ConsumerEvent::~ConsumerEvent(){CloseHandle(handle_);}
std::shared_ptr<const ConsumerEnd> Bridge::end_consumer(const Consumer& c,std::shared_ptr<ConsumerEvent> event){
 if(!owns_consumer(c)||impl_->consumer11->GetCompletedValue()==UINT64_MAX)return {};
 if(event&&(event->bound_||WaitForSingleObject(event->handle_,0)!=WAIT_TIMEOUT))return {};
 auto* s=impl_->find(c.ticket);
 // This original private fence has one exclusive submission-order cursor. Reserve
 // only here, after the caller's last copy, with no callback/yield before Signal.
 const auto value=impl_->consumerCursor.reserve();if(!value)return {};
 s->consumer=value;s->endEvent=event;
 const auto receipt=std::make_shared<const ConsumerEnd>(ConsumerEnd{c,value,event});
 if(event){
  event->bound_=true;
  if(FAILED(impl_->consumer11->SetEventOnCompletion(value,event->handle_))){s->failed=true;return {};}
 }
 if(FAILED(impl_->graphics->Signal(impl_->consumer11.Get(),value))){s->failed=true;return {};}
 s->endReceipt=receipt;impl_->graphics->Flush();s->consumerOpen=false;return receipt;
}
bool Bridge::cancel(const Ticket& t){if(!impl_||!impl_->on_graphics())return false;
 auto* s=impl_->find(t);if(!s)return false;s->cancelled=true;return true;}
Ownership Bridge::ownership(size_t index)const{return impl_?impl_->snapshot(index):Ownership{std::nullopt,WorkPhase::Empty,0,0,0,0,0,0,false,false,false};}
bool Bridge::quiescent()const{if(!impl_)return closed_;for(size_t i=0;i<3;++i)if(!reusable(impl_->snapshot(i)))return false;return true;}
bool Bridge::close(){if(!impl_)return closed_;if(!impl_->on_graphics()||!quiescent())return false;impl_.reset();closed_=true;return true;}
ID3D12Fence* Bridge::producer_fence()const{return impl_?impl_->producer12.Get():nullptr;}
ID3D12Fence* Bridge::worker_fence()const{return impl_?impl_->worker12.Get():nullptr;}
ID3D11Fence* Bridge::consumer_fence()const{return impl_?impl_->consumer11.Get():nullptr;}
} // namespace nr::interop
