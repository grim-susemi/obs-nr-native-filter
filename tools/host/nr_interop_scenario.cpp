// SPDX-License-Identifier: GPL-3.0-only
#include "nr_interop_scenario.h"
#include "interop.h"
#include <obs.h>
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <stdexcept>
namespace {
using namespace nr;
using namespace nr::interop;
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
void checked(HRESULT hr){require(SUCCEEDED(hr),"QA interop HRESULT failed");}
struct Event {HANDLE handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 Event(){require(handle!=nullptr,"QA event creation failed");}~Event(){CloseHandle(handle);}
 void await(){require(WaitForSingleObject(handle,15000)==WAIT_OBJECT_0,"QA exact event timeout");}
 void signal(){require(SetEvent(handle)!=FALSE,"QA event signal failed");}
};
std::vector<std::byte> shader(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary|std::ios::ate);
 require(static_cast<bool>(f),"interop shader missing");const auto size=f.tellg();require(size>0,"empty shader");
 std::vector<std::byte> bytes(static_cast<size_t>(size));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),size);
 require(static_cast<bool>(f),"shader read failed");return bytes;}
uint64_t hash(const std::vector<uint8_t>& bytes){uint64_t h=14695981039346656037ull;for(auto b:bytes)h=(h^b)*1099511628211ull;return h;}
std::vector<uint8_t> pattern(PixelFormat format){const size_t stride=format==PixelFormat::Rgba16Float?8:4;
 std::vector<uint8_t> result(16*16*stride);
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){auto* p=result.data()+(y*16+x)*stride;
  if(format==PixelFormat::Rgba16Float){const uint16_t rgb[]{0xb400,0x2400,0x3800,0x3c00,0x4200,0x4900,0x4c00};
   const uint16_t alpha[]{0,0x3400,0x3800,0x3c00};const auto a=alpha[y%4];
   const uint16_t values[]{static_cast<uint16_t>(a?rgb[x%7]:0),static_cast<uint16_t>(a?rgb[(x+y+2)%7]:0),static_cast<uint16_t>(a?rgb[(x+4)%7]:0),a};std::memcpy(p,values,8);
  }else if(format==PixelFormat::Rgb10A2){const unsigned a=y%4;const uint32_t r=(x*61+7)%1024*a/3,g=(y*59+11)%1024*a/3,b=(x*17+y*23+3)%1024*a/3;
   const uint32_t packed=r|(g<<10)|(b<<20)|(a<<30);std::memcpy(p,&packed,4);
  }else{const uint8_t alpha[]{0,64,128,255};const unsigned a=alpha[y%4];
   const uint8_t r=static_cast<uint8_t>((x*15+7)%256*a/255),g=static_cast<uint8_t>((y*13+11)%256*a/255),b=static_cast<uint8_t>((x*7+y*5+3)%256*a/255);
   p[0]=format==PixelFormat::Bgra8?b:r;p[1]=g;p[2]=format==PixelFormat::Bgra8?r:b;p[3]=static_cast<uint8_t>(a);
  }
 }return result;
}
struct GraphicsState {
 Ptr<ID3D11RenderTargetView> target;Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;Ptr<ID3D11RasterizerState> raster;
 D3D11_PRIMITIVE_TOPOLOGY topology{};std::array<D3D11_VIEWPORT,16> viewport{};UINT count=16;
 explicit GraphicsState(ID3D11DeviceContext* c){c->OMGetRenderTargets(1,&target,nullptr);c->VSGetShader(&vs,nullptr,nullptr);c->PSGetShader(&ps,nullptr,nullptr);c->RSGetState(&raster);c->IAGetPrimitiveTopology(&topology);c->RSGetViewports(&count,viewport.data());}
 bool equal(const GraphicsState& other)const{return target.Get()==other.target.Get()&&vs.Get()==other.vs.Get()&&ps.Get()==other.ps.Get()&&raster.Get()==other.raster.Get()&&topology==other.topology&&count==other.count&&std::memcmp(viewport.data(),other.viewport.data(),sizeof(D3D11_VIEWPORT)*count)==0;}
};
enum class Action {None,Init,RawProbe,Verify,Reuse,Cleanup};
struct Harness {
 // Cleanup/event ownership is established before any device, queue, resource or worker.
 Event acted,submitted,discardTrigger,workerExited,workerComplete,consumerComplete,producerComplete;
 std::atomic<Action> action{Action::None};std::atomic<bool> failed{false},workerFailed{false};
 PixelFormat format;unsigned generation;std::vector<std::byte> vs,ps;std::vector<uint8_t> expected;
 gs_texture_t* source=nullptr;Ptr<ID3D11Device> device11;Ptr<ID3D11DeviceContext> graphics;
 Ptr<ID3D11Texture2D> target,stage;std::shared_ptr<Context> context;std::unique_ptr<Bridge> bridge;Ptr<ID3D12Fence> gate;
 std::array<std::optional<Ticket>,3> tickets;std::optional<Ticket> resumed;
 std::thread worker;uint64_t captureClock=0,progressClock=0;unsigned rawErrors=0,outputErrors=0;bool restored=true;
 uint64_t rawHash=0,outputHash=0;
 Harness(PixelFormat f,unsigned g,const std::filesystem::path& root):format(f),generation(g),
  vs(shader(root/"bin/64bit/nr_interop_vertex.cso")),ps(shader(root/"bin/64bit/nr_interop_pixel.cso")),expected(pattern(f)){}
 JobKey key(uint64_t serial)const{return {{71,1,serial},1,generation,serial};}
 void execute(Action next){ResetEvent(acted.handle);action.store(next);acted.await();require(!failed,"graphics action failed");}
 std::vector<uint8_t> readback(){graphics->CopyResource(stage.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};
  checked(graphics->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));const size_t stride=format==PixelFormat::Rgba16Float?128:64;
  std::vector<uint8_t> result(expected.size());for(unsigned y=0;y<16;++y)std::memcpy(result.data()+y*stride,static_cast<const uint8_t*>(map.pData)+y*map.RowPitch,stride);
  graphics->Unmap(stage.Get(),0);return result;}
 unsigned errors(const std::vector<uint8_t>& data)const{unsigned count=0;for(size_t i=0;i<data.size();++i)if(data[i]!=expected[i])++count;return count;}
 void init(){
  require(gs_get_device_type()==GS_DEVICE_DIRECT3D_11,"interop requires actual OBS D3D11");
  const gs_color_format formats[]{GS_BGRA,GS_RGBA,GS_RGBA16F,GS_R10G10B10A2};
  const auto gsFormat=formats[generation-1];source=gs_texture_create(16,16,gsFormat,1,nullptr,GS_DYNAMIC);require(source,"OBS fixture texture creation failed");
  gs_texture_set_image(source,expected.data(),format==PixelFormat::Rgba16Float?128:64,false);
  auto* borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(source));require(borrowed,"actual OBS texture object missing");borrowed->GetDevice(&device11);device11->GetImmediateContext(&graphics);
  D3D11_TEXTURE2D_DESC actual{};borrowed->GetDesc(&actual);
  std::cout<<"NR_INTEROP_SOURCE generation="<<generation<<" format="<<static_cast<unsigned>(actual.Format)<<" declared="<<static_cast<unsigned>(source_format(format))<<" width="<<actual.Width<<" height="<<actual.Height<<" mip="<<actual.MipLevels<<" array="<<actual.ArraySize<<" samples="<<actual.SampleDesc.Count<<std::endl;
  context=Context::create(device11.Get());const FrameDescriptor d{{16,16},{0,0,16,16},format,
   format==PixelFormat::Rgba16Float?ColorSpace::ScRgbLinear:format==PixelFormat::Rgb10A2?ColorSpace::Rec2020Pq:ColorSpace::Rec709Srgb,AlphaMode::Premultiplied,203};
  bridge=std::make_unique<Bridge>(context,generation,d,vs,ps);checked(context->device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
  D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.ArraySize=td.MipLevels=td.SampleDesc.Count=1;td.Format=source_format(format);td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  checked(device11->CreateTexture2D(&td,nullptr,&target));td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;checked(device11->CreateTexture2D(&td,nullptr,&stage));
  const GraphicsState before(graphics.Get());for(size_t i=0;i<3;++i){const auto captured=bridge->capture(key(i+1),borrowed);require(captured.has_value(),"borrowed OBS texture capture refused");tickets[i].emplace(*captured);}
  require(!bridge->capture(key(4),borrowed),"fourth live slot allocated");restored=before.equal(GraphicsState(graphics.Get()));
  std::vector<uint8_t> changed(expected.size(),0);gs_texture_set_image(source,changed.data(),format==PixelFormat::Rgba16Float?128:64,false);captureClock=obs_get_video_frame_time();
  Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter> warp;Ptr<ID3D12Device> software;Ptr<ID3D12CommandQueue> softwareQueue;
  checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));checked(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&software)));
  D3D12_COMMAND_QUEUE_DESC q{};checked(software->CreateCommandQueue(&q,IID_PPV_ARGS(&softwareQueue)));
  require(!compatible_context(device11.Get(),software.Get(),softwareQueue.Get()),"actual wrong-adapter WARP admitted");
  Ptr<ID3D12Resource> invalid;const HRESULT handleResult=open_shared_resource(context->device(),INVALID_HANDLE_VALUE,&invalid);require(FAILED(handleResult)&&!invalid,"invalid shared handle admitted");
  std::cout<<"NR_INTEROP_IDENTITY "<<generation<<" renderer_luid="<<context->luid().HighPart<<":"<<context->luid().LowPart<<" worker_luid="<<context->device()->GetAdapterLuid().HighPart<<":"<<context->device()->GetAdapterLuid().LowPart<<" canonical_queue=true wrong_adapter_refused=true shared_handle_hr="<<static_cast<uint32_t>(handleResult)<<std::endl;
 }
 void raw_probe(){
  require(bridge->worker_fence()->GetCompletedValue()==0,"controlled unfinished work unexpectedly completed");
  progressClock=obs_get_video_frame_time();require(progressClock>captureClock,"renderer did not progress while worker held");
  const GraphicsState before(graphics.Get());
  for(const auto& ticket:tickets){require(!bridge->acquire_output(*ticket),"unfinished worker published");const auto raw=bridge->acquire_raw(*ticket);require(raw.has_value(),"raw ownership unavailable");
   require(bridge->copy_to(*raw,target.Get())&&bridge->end_consumer(*raw),"raw consumer failed");const auto data=readback();rawErrors+=errors(data);rawHash=hash(data);}
  require(bridge->cancel(*tickets[2]),"cancel failed");require(!bridge->quiescent()&&!bridge->close(),"cancel released unfinished worker");
  auto failedFence=bridge->ownership(0);failedFence.workerDone=UINT64_MAX;
  require(!publishable(failedFence,tickets[0]->key)&&!reusable(failedFence),"failed-fence sentinel admitted");
  auto* borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(source));require(!bridge->capture(key(4),borrowed),"unfinished worker slot reused");
  restored=restored&&before.equal(GraphicsState(graphics.Get()));require(rawErrors==0,"immutable raw differs after source mutation");
 }
 void verify(){
  std::vector<Consumer> consumers;for(size_t i=0;i<3;++i){const auto c=i==2?bridge->acquire_raw(*tickets[i]):bridge->acquire_output(*tickets[i]);require(c.has_value(),"completed consumer missing");consumers.push_back(*c);}
  require(!bridge->acquire_output(*tickets[2]),"cancelled completion resurrected");
  require(bridge->consumer_fence()->GetCompletedValue()<consumers.back().completionValue,"unsignalled consumer fence already complete");
  require(!bridge->quiescent()&&!bridge->close(),"incomplete consumer generation released");
  auto* borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(source));require(!bridge->capture(key(4),borrowed),"incomplete consumer slot reused");
  const GraphicsState before(graphics.Get());
  for(const auto& c:consumers){require(bridge->copy_to(c,target.Get()),"output conversion failed");require(bridge->end_consumer(c),"consumer signal failed");const auto data=readback();outputErrors+=errors(data);outputHash=hash(data);}
  restored=restored&&before.equal(GraphicsState(graphics.Get()));require(outputErrors==0&&restored,"roundtrip numeric/alpha/state mismatch");
 }
 void reuse(){
  require(bridge->quiescent(),"completed physical ownership retained");auto* borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(source));
  resumed.emplace(bridge->capture(key(4),borrowed).value());require(!bridge->acquire_output(*tickets[0]),"old reused-slot ticket admitted");
  require(bridge->cancel(*resumed),"resumed cancellation failed");require(!bridge->close(),"unsubmitted recording released");
 }
 void cleanup(){
  require(!worker.joinable(),"worker still live at cleanup");
  const auto producerDone=bridge->producer_fence()->GetCompletedValue(),workerDone=bridge->worker_fence()->GetCompletedValue(),consumerDone=bridge->consumer_fence()->GetCompletedValue();
  require(producerDone==4&&workerDone==3&&consumerDone==6,"actual fence lineage mismatch");
  require(bridge->close(),"physical ownership unresolved at cleanup");bridge.reset();gate.Reset();target.Reset();stage.Reset();
  gs_texture_destroy(source);source=nullptr;graphics.Reset();device11.Reset();context.reset();
  const char* names[]{"BGRA8","RGBA8","RGBA16F","RGB10A2"};
  std::cout<<"NR_INTEROP_PATTERN {"<<char(34)<<"format"<<char(34)<<":"<<char(34)<<names[generation-1]<<char(34)
   <<","<<char(34)<<"raw_errors"<<char(34)<<":"<<rawErrors<<","<<char(34)<<"output_errors"<<char(34)<<":"<<outputErrors
   <<","<<char(34)<<"state_restored"<<char(34)<<":"<<(restored?"true":"false")
   <<","<<char(34)<<"expected_fnv1a64"<<char(34)<<":"<<char(34)<<hash(expected)<<char(34)
   <<","<<char(34)<<"raw_fnv1a64"<<char(34)<<":"<<char(34)<<rawHash<<char(34)
   <<","<<char(34)<<"output_fnv1a64"<<char(34)<<":"<<char(34)<<outputHash<<char(34)
   <<","<<char(34)<<"capture_clock"<<char(34)<<":"<<captureClock<<","<<char(34)<<"progress_clock"<<char(34)<<":"<<progressClock
   <<","<<char(34)<<"producer_fence_completed"<<char(34)<<":"<<producerDone<<","<<char(34)<<"worker_fence_completed"<<char(34)<<":"<<workerDone
   <<","<<char(34)<<"consumer_fence_completed"<<char(34)<<":"<<consumerDone<<","<<char(34)<<"cleanup"<<char(34)<<":true}"<<std::endl;
 }
};
void draw(void* opaque,uint32_t,uint32_t){auto& h=*static_cast<Harness*>(opaque);const auto action=h.action.exchange(Action::None);if(action==Action::None)return;
 try{switch(action){case Action::Init:h.init();break;case Action::RawProbe:h.raw_probe();break;case Action::Verify:h.verify();break;case Action::Reuse:h.reuse();break;case Action::Cleanup:h.cleanup();break;default:break;}}
 catch(const std::exception& e){h.failed=true;std::cout<<"NR_INTEROP_FAILURE graphics "<<e.what()<<std::endl;}
 h.acted.signal();
}
void worker(Harness* h){
 try{checked(h->context->queue()->Wait(h->gate.Get(),1));for(const auto& t:h->tickets)require(h->bridge->record_copy(*t),"production worker submission failed");}
 catch(const std::exception& e){h->workerFailed=true;std::cout<<"NR_INTEROP_FAILURE worker "<<e.what()<<std::endl;}
 h->submitted.signal();
 try{h->discardTrigger.await();if(h->resumed)require(h->bridge->discard_recording(*h->resumed),"resumed recording discard failed");}
 catch(const std::exception& e){h->workerFailed=true;std::cout<<"NR_INTEROP_FAILURE discard "<<e.what()<<std::endl;}
 h->workerExited.signal();
}
} // namespace
bool nr_run_interop_scenario(const std::filesystem::path& root){
 std::cout<<"NR_INTEROP_CLEANUP_REGISTERED owned_events_context_queues_generations_only"<<std::endl;
 const PixelFormat formats[]{PixelFormat::Bgra8,PixelFormat::Rgba8,PixelFormat::Rgba16Float,PixelFormat::Rgb10A2};
 for(unsigned i=0;i<4;++i){auto h=std::make_unique<Harness>(formats[i],i+1,root);bool callback=false;
  try{
   obs_add_main_render_callback(draw,h.get());callback=true;h->execute(Action::Init);
   // Fence subscriptions exist BEFORE worker submissions and before releasing its deterministic GPU gate.
   checked(h->bridge->worker_fence()->SetEventOnCompletion(3,h->workerComplete.handle));
   checked(h->bridge->consumer_fence()->SetEventOnCompletion(6,h->consumerComplete.handle));
   checked(h->bridge->producer_fence()->SetEventOnCompletion(4,h->producerComplete.handle));
   h->worker=std::thread(worker,h.get());h->submitted.await();require(!h->workerFailed,"worker failed before raw probe");
   h->execute(Action::RawProbe);checked(h->gate->Signal(1));h->workerComplete.await();
   h->execute(Action::Verify);h->consumerComplete.await();h->execute(Action::Reuse);h->producerComplete.await();
   h->discardTrigger.signal();h->workerExited.await();h->worker.join();require(!h->workerFailed,"worker discard failed");
   h->execute(Action::Cleanup);obs_remove_main_render_callback(draw,h.get());callback=false;
  }catch(const std::exception& e){
   std::cout<<"NR_INTEROP_FAILURE host "<<e.what()<<std::endl;
   // Release only our controlled test dependency. Never terminate a worker or force-free unresolved GPU ownership.
   if(h->gate){const auto hr=h->gate->Signal(1);if(FAILED(hr))std::cout<<"NR_INTEROP_CLEANUP_GATE_FAILED "<<static_cast<uint32_t>(hr)<<std::endl;}
   if(h->worker.joinable()){h->discardTrigger.signal();
    if(WaitForSingleObject(h->workerExited.handle,15000)==WAIT_OBJECT_0)h->worker.join();}
   if(callback)obs_remove_main_render_callback(draw,h.get());
   if(h->worker.joinable()||(h->bridge&&!h->bridge->quiescent())){(void)h.release();std::cout<<"NR_INTEROP_RETAINED unresolved=true"<<std::endl;}
   else{obs_enter_graphics();gs_texture_destroy(h->source);h->source=nullptr;obs_leave_graphics();}
   return false;
  }
 }
 std::cout<<"NR_INTEROP_SCOPE transport=physical same_adapter=true model=not_loaded recording=not_tested HDR=transport_only cleanup=complete"<<std::endl;
 return true;
}
