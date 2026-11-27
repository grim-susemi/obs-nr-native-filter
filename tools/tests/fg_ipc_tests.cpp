// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - A single owned two-process GPU scenario and its negative
// variants share this executable entry point. The assigned handoff permits
// only this new test source; bootstrap is not a general process framework.
#include "fg/fg_ipc.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <vector>

using namespace nr::fg::ipc;
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
void expect(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr,const char* why){if(FAILED(hr))throw Error(Status::ProtocolError,hr,why);}
struct Handle {
 HANDLE value=nullptr;
 ~Handle(){if(value)CloseHandle(value);}
 Handle()=default;
 Handle(const Handle&)=delete;
 Handle& operator=(const Handle&)=delete;
};
struct View {
 void* address=nullptr;
 ~View(){if(address)UnmapViewOfFile(address);}
 template<class T>T* get()const{return static_cast<T*>(address);}
 void map(HANDLE h,size_t bytes){address=MapViewOfFile(h,FILE_MAP_ALL_ACCESS,0,0,bytes);expect(address!=nullptr,"test mapping");}
};
void signal(HANDLE h){expect(SetEvent(h)!=FALSE,"signal exact event");}
void await(HANDLE event,HANDLE peer){
 const HANDLE events[]{event,peer};
 expect(WaitForMultipleObjects(2,events,FALSE,10000)==WAIT_OBJECT_0,"subscribed event or peer death before deadline");
}
void finish(HANDLE process){
 expect(WaitForSingleObject(process,10000)==WAIT_OBJECT_0,"owned child exit deadline");
 DWORD code=1;expect(GetExitCodeProcess(process,&code)!=FALSE&&code==0,"owned child exit code");
}
enum class Mode:uint32_t { Frames, Version, Adapter, Crash, Rejected };
struct Bootstrap {
 wchar_t mapping[256]{};
 uint64_t generation=0,parentCreation=0;
 DWORD parentPid=0;
 LUID adapter{};
 Mode mode=Mode::Frames;
 uint32_t frames=0;
 HANDLE parent=nullptr,start=nullptr,go=nullptr,step=nullptr,reached=nullptr,resume=nullptr;
};
struct Child {
 Handle control,parent,start,go,step,reached,resume,process,thread,job;
 View view;Peer owned{};
 Child(Mode mode,uint32_t frames){
  SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
  control.value=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(Bootstrap)),nullptr);
  expect(control.value!=nullptr,"bootstrap allocation");view.map(control.value,sizeof(Bootstrap));
  for(auto* h:{&start,&go,&reached,&resume}){h->value=CreateEventW(&sa,TRUE,FALSE,nullptr);expect(h->value!=nullptr,"bootstrap event");}
  step.value=CreateSemaphoreW(&sa,0,1000,nullptr);expect(step.value!=nullptr,"exact counted progress");
  expect(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&parent.value,
   SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_DUP_HANDLE,TRUE,0)!=FALSE,"inherited parent identity");
  auto& b=*view.get<Bootstrap>();const auto self=identity(parent.value);
  b.parent=parent.value;b.parentPid=self.pid;b.parentCreation=self.creation;b.start=start.value;b.go=go.value;
  b.step=step.value;b.reached=reached.value;b.resume=resume.value;b.mode=mode;b.frames=frames;
  std::array<HANDLE,8> handles{control.value,parent.value,start.value,go.value,step.value,reached.value,resume.value,nullptr};
  SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
  std::vector<std::byte> storage(bytes);auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  expect(InitializeProcThreadAttributeList(attributes,1,0,&bytes)!=FALSE,"child attribute allocation");
  struct Attributes {LPPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}}cleanup{attributes};
  expect(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),7*sizeof(HANDLE),nullptr,nullptr)!=FALSE,"explicit inherited handles");
  wchar_t executable[32768]{};expect(GetModuleFileNameW(nullptr,executable,32768)!=0,"test executable path");
  std::wstring command=L"\""+std::wstring(executable)+L"\" --child "+std::to_wstring(reinterpret_cast<uintptr_t>(control.value));
  STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=attributes;PROCESS_INFORMATION pi{};
  job.value=CreateJobObjectW(nullptr,nullptr);expect(job.value!=nullptr,"owned child job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  expect(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=FALSE,"owned job kill on close");
  expect(CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,
   nullptr,nullptr,&startup.StartupInfo,&pi)!=FALSE,"spawn same executable child");
  process.value=pi.hProcess;thread.value=pi.hThread;owned=identity(process.value);
  if(!AssignProcessToJobObject(job.value,process.value)){
   expect(TerminateProcess(process.value,93)!=FALSE,"clean unassigned owned child");expect(WaitForSingleObject(process.value,5000)==WAIT_OBJECT_0,"unassigned owned child exit");
   throw std::runtime_error("assign owned child job");
  }
  expect(ResumeThread(thread.value)!=std::numeric_limits<DWORD>::max(),"resume assigned child");
 }
 ~Child(){
  // The job contains only the process spawned here; never enumerate or kill a PID.
  if(process.value&&WaitForSingleObject(process.value,0)==WAIT_TIMEOUT){
   if(job.value){CloseHandle(job.value);job.value=nullptr;}
   WaitForSingleObject(process.value,5000);
  }
 }
 void offer(const Offer& o,LUID adapter){
  auto& b=*view.get<Bootstrap>();expect(o.mapping.size()<std::size(b.mapping),"offer name capacity");
  std::copy(o.mapping.begin(),o.mapping.end(),b.mapping);b.mapping[o.mapping.size()]=L'\0';b.generation=o.generation;b.adapter=adapter;signal(start.value);
 }
 double kill(){
  const auto actual=identity(process.value);expect(actual.pid==owned.pid&&actual.creation==owned.creation,"only exact owned child may be killed");
  LARGE_INTEGER before{},after{},frequency{};expect(QueryPerformanceFrequency(&frequency)!=FALSE,"QPC frequency");
  expect(QueryPerformanceCounter(&before)!=FALSE,"QPC before owned kill");
  expect(TerminateProcess(process.value,91)!=FALSE,"terminate exact owned child");
  expect(WaitForSingleObject(process.value,500)==WAIT_OBJECT_0,"owned process death detected within 500 ms");
  expect(QueryPerformanceCounter(&after)!=FALSE,"QPC after owned kill");
  const double milliseconds=1000.0*static_cast<double>(after.QuadPart-before.QuadPart)/static_cast<double>(frequency.QuadPart);
  expect(milliseconds<=500.0,"death detection elapsed <= 500 ms");return milliseconds;
 }
};
Shape shape(){
 Shape s{16,16,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,{},true};
 std::copy_n(L"IPC_TEST",9,s.sourceMonitor.begin());return s;
}
uint32_t pixel(uint64_t serial,uint32_t x,uint32_t y){return static_cast<uint32_t>(serial)*0x010101u+x*257u+y*65537u;}
constexpr uint64_t continuous_frames=1000, first_frame_after_switch=5;
struct Renderer {
 Ptr<ID3D11Device> device;Ptr<ID3D11DeviceContext> context;Ptr<ID3D11Texture2D> source;LUID adapter{};
 Renderer(){
  const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
  check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,&device,nullptr,&context),"hardware D3D11");
  Ptr<IDXGIDevice> dxgi;Ptr<IDXGIAdapter> a;DXGI_ADAPTER_DESC desc{};
  check(device.As(&dxgi),"renderer DXGI");check(dxgi->GetAdapter(&a),"renderer adapter");check(a->GetDesc(&desc),"renderer LUID");adapter=desc.AdapterLuid;
  D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=16;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=shape().format;
  d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;check(device->CreateTexture2D(&d,nullptr,&source),"test source");
 }
 void send(Producer& producer,uint64_t serial,bool discontinuity=false){
  std::array<uint32_t,256> data{};for(uint32_t y=0;y<16;++y)for(uint32_t x=0;x<16;++x)data[y*16+x]=pixel(serial,x,y);
  context->UpdateSubresource(source.Get(),0,nullptr,data.data(),16*sizeof(uint32_t),0);
  expect(producer.publish(source.Get(),serial*1000,serial*1000+7,discontinuity)==Status::Ok,"published test frame without loss");
 }
};
struct Reader {
 Ptr<ID3D12Device> device;Ptr<ID3D12CommandQueue> queue;Ptr<ID3D12Resource> buffer;
 Reader(LUID adapter){
  Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter1> a;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"consumer factory");check(factory->EnumAdapterByLuid(adapter,IID_PPV_ARGS(&a)),"exact consumer adapter");
  check(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"hardware D3D12");
  D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"consumer queue");
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=4096;d.Height=d.SampleDesc.Count=1;d.DepthOrArraySize=d.MipLevels=1;
  d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer)),"private readback");
 }
 void checksum(uint64_t serial){
  void* bytes=nullptr;D3D12_RANGE range{0,4096};check(buffer->Map(0,&range,&bytes),"readback map after exact fence");
  struct Unmap {ID3D12Resource* resource;~Unmap(){D3D12_RANGE empty{};resource->Unmap(0,&empty);}}unmap{buffer.Get()};
  uint64_t actual=0,expected=0;
  for(uint32_t y=0;y<16;++y)for(uint32_t x=0;x<16;++x){
   uint32_t value=0;std::memcpy(&value,static_cast<const std::byte*>(bytes)+y*256+x*4,4);
   expect(value==pixel(serial,x,y),"exact pixel mismatch");actual+=value;expected+=pixel(serial,x,y);
  }
  expect(actual==expected,"exact frame checksum");
 }
};
uint32_t mv_pixel(uint64_t serial,uint32_t x,uint32_t y){
 const auto u=static_cast<uint16_t>((x*3u+y*5u+serial*7u)&0xffffu);
 const auto v=static_cast<uint16_t>((x*11u+y*13u+serial*17u)&0xffffu);
 return static_cast<uint32_t>(u)|(static_cast<uint32_t>(v)<<16);
}
struct FlowGpu {
 Ptr<ID3D11Device> producer;Ptr<ID3D11DeviceContext> context;
 Ptr<ID3D11Texture2D> after,before,motion;
 Ptr<ID3D12Device> device;Ptr<ID3D12CommandQueue> queue;LUID adapter{};
 static constexpr uint32_t width=1920,height=1080,guideWidth=1344,guideHeight=756;
 FlowGpu(){
  const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
  check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,&producer,nullptr,&context),"flow D3D11");
  Ptr<IDXGIDevice> dxgi;Ptr<IDXGIAdapter> adapter0;DXGI_ADAPTER_DESC desc{};
  check(producer.As(&dxgi),"flow DXGI");check(dxgi->GetAdapter(&adapter0),"flow adapter");check(adapter0->GetDesc(&desc),"flow LUID");
  adapter=desc.AdapterLuid;
  after=texture(width,height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);
  before=texture(width,height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);
  motion=texture(guideWidth,guideHeight,DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
  Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter1> adapter1;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"flow factory");
  check(factory->EnumAdapterByLuid(adapter,IID_PPV_ARGS(&adapter1)),"flow consumer adapter");
  check(D3D12CreateDevice(adapter1.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"flow D3D12");
  D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"flow queue");
 }
 Ptr<ID3D11Texture2D> texture(uint32_t w,uint32_t h,DXGI_FORMAT format,UINT bind){
  D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;
  d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;Ptr<ID3D11Texture2D> result;
  check(producer->CreateTexture2D(&d,nullptr,&result),"flow source texture");return result;
 }
 Ptr<ID3D12Resource> resource(uint32_t w,uint32_t h,DXGI_FORMAT format){
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
  d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;Ptr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result)),"flow destination");
  return result;
 }
 Ptr<ID3D12Resource> buffer(UINT64 bytes){
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=d.SampleDesc.Count=1;
  d.DepthOrArraySize=d.MipLevels=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;Ptr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"flow readback");
  return result;
 }
 void fill(uint64_t serial){
  std::vector<uint32_t> color(static_cast<size_t>(width)*height);
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)color[static_cast<size_t>(y)*width+x]=pixel(serial,x,y);
  context->UpdateSubresource(after.Get(),0,nullptr,color.data(),width*sizeof(uint32_t),0);
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)color[static_cast<size_t>(y)*width+x]=pixel(serial-1,x,y);
  context->UpdateSubresource(before.Get(),0,nullptr,color.data(),width*sizeof(uint32_t),0);
  std::vector<uint32_t> guide(static_cast<size_t>(guideWidth)*guideHeight);
  for(uint32_t y=0;y<guideHeight;++y)for(uint32_t x=0;x<guideWidth;++x)guide[static_cast<size_t>(y)*guideWidth+x]=mv_pixel(serial,x,y);
  context->UpdateSubresource(motion.Get(),0,nullptr,guide.data(),guideWidth*sizeof(uint32_t),0);
 }
};
struct ColorReadback {
 Ptr<ID3D12Resource> buffer;Ptr<ID3D12CommandAllocator> allocator;Ptr<ID3D12GraphicsCommandList> list;
 Ptr<ID3D12Fence> fence;HANDLE event=nullptr;D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};uint64_t value=0;
 ColorReadback(ID3D12Device* device,uint32_t w,uint32_t h){
  D3D12_RESOURCE_DESC source{};source.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;source.Width=w;source.Height=h;
  source.DepthOrArraySize=source.MipLevels=1;source.Format=DXGI_FORMAT_R8G8B8A8_UNORM;source.SampleDesc.Count=1;
  UINT64 bytes=0;device->GetCopyableFootprints(&source,0,1,0,&layout,nullptr,nullptr,&bytes);
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=desc.SampleDesc.Count=1;
  desc.DepthOrArraySize=desc.MipLevels=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer)),"colour readback");
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"colour readback allocator");
  check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"colour readback list");
  check(list->Close(),"colour readback close");
  check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"colour readback fence");
  event=CreateEventW(nullptr,FALSE,FALSE,nullptr);expect(event!=nullptr,"colour readback event");
 }
 ~ColorReadback(){if(event)CloseHandle(event);}
 void read(ID3D12CommandQueue* queue,ID3D12Resource* source){
  check(allocator->Reset(),"colour readback allocator reset");check(list->Reset(allocator.Get(),nullptr),"colour readback list reset");
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition={source,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
  list->ResourceBarrier(1,&b);
  D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=layout;
  list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;
  list->ResourceBarrier(1,&b);check(list->Close(),"colour readback submit close");
  ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
  check(queue->Signal(fence.Get(),++value),"colour readback signal");
  check(fence->SetEventOnCompletion(value,event),"colour readback completion");
  expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"colour readback deadline");
 }
 void verify(uint32_t w,uint32_t h,uint64_t serial,bool previous){
  void* bytes=nullptr;check(buffer->Map(0,nullptr,&bytes),"colour readback map");
  struct Unmap {ID3D12Resource* resource;~Unmap(){D3D12_RANGE empty{};resource->Unmap(0,&empty);}}unmap{buffer.Get()};
  for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
   uint32_t actual=0;
   std::memcpy(&actual,static_cast<const std::byte*>(bytes)+static_cast<size_t>(y)*layout.Footprint.RowPitch+x*4,4);
   expect(actual==pixel(previous?serial-1:serial,x,y),"exact split colour texel");
  }
 }
};
void flow_contract(){
 // Given a split 1920x1080 generation with a 1344x756 guide ring, When two
 // adjacent NrOwned+Valid frames cross the real transport, Then the guide extent
 // stays independent of colour, pairing gates shared MV exposure, and every
 // colour and MV texel arrives exactly.
 FlowGpu gpu;const auto self=identity(GetCurrentProcess());
 const Shape shape{FlowGpu::width,FlowGpu::height,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,{},true,true,{FlowGpu::guideWidth,FlowGpu::guideHeight}};
 const auto name=L"Local\\nr-fg-ipc-flow-"+std::to_wstring(self.pid)+L"-"+std::to_wstring(self.creation);
 Producer producer(gpu.producer.Get(),name,shape);
 const Peer peer{GetCurrentProcess(),self.pid,self.creation};
 const auto offer=producer.connect(peer);
 Consumer consumer(gpu.queue.Get(),offer,peer);
 expect(producer.status()==Status::Ok,"shared flow handshake ready");
 const auto key_for=[&](uint64_t serial){
  return FrameKey{0x51u,static_cast<uint32_t>(producer.generation()),serial,serial*16666667u,1,2};
 };
 for(uint64_t serial=1;serial<=2;++serial){
  gpu.fill(serial);
  FlowSample flow{FlowOwner::NrOwned,FlowProvenance::Valid,4.0f/3.0f,1.0f,40+serial,41+serial,false,false,true};
  flow.prevColor=serial==1?key_for(40):key_for(serial-1);
  flow.currColor=key_for(serial);
  const FlowBundle bundle{gpu.motion.Get(),key_for(serial),flow};
  expect(producer.publish(gpu.after.Get(),serial*1000,serial*1000+7,false,serial*16666667,gpu.before.Get(),50,false,
   bundle)==Status::Ok,"shared flow publish");
 }
 Handle done;done.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);expect(done.value!=nullptr,"flow completion event");
 const auto destination=[](auto& gpu){return gpu.resource(FlowGpu::width,FlowGpu::height,DXGI_FORMAT_R8G8B8A8_UNORM);};
 Ptr<ID3D12Resource> after1=destination(gpu),before1=destination(gpu),after2=destination(gpu),before2=destination(gpu);
 D3D12_RESOURCE_DESC motion{};motion.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;motion.Width=FlowGpu::guideWidth;
 motion.Height=FlowGpu::guideHeight;motion.DepthOrArraySize=motion.MipLevels=1;motion.Format=DXGI_FORMAT_R16G16_FLOAT;motion.SampleDesc.Count=1;
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT motionLayout{};UINT64 motionBytes=0;
 gpu.device->GetCopyableFootprints(&motion,0,1,0,&motionLayout,nullptr,nullptr,&motionBytes);
 auto motionReadback=gpu.buffer(motionBytes);
 const auto first=consumer.copy_next(after1.Get(),before1.Get(),nullptr);
 expect(first.status==Status::Ok&&first.current&&!first.previous&&!first.hasValidSharedMv,"first flow frame is an unpaired original");
 expect(first.current->frame.key==key_for(1),"first displayed colour key survives the wire");
 expect(consumer.arm_return(*first.current,done.value)==Status::Ok,"first flow return subscription");
 await(done.value,consumer.producer_process());
 const auto second=consumer.copy_next(after2.Get(),before2.Get(),motionReadback.Get());
 expect(second.status==Status::Ok&&second.current&&second.previous&&second.hasValidSharedMv,"second adjacent flow frame exposes shared MV");
 expect(consumer.arm_return(*second.current,done.value)==Status::Ok,"second flow return subscription");
 await(done.value,consumer.producer_process());
 expect(second.current->frame.shape.width==FlowGpu::width&&second.current->frame.shape.guides.width==FlowGpu::guideWidth&&
  second.current->frame.shape.guides.height==FlowGpu::guideHeight&&second.current->frame.flow.mvScaleX==4.0f/3.0f&&
  second.current->frame.flow.owner==FlowOwner::NrOwned&&second.current->frame.flow.provenance==FlowProvenance::Valid&&
  second.current->frame.key==key_for(2)&&second.current->frame.flow.prevColor==key_for(1)&&
  second.current->frame.flow.currColor==key_for(2),
  "guide extent, provenance, and both displayed colour keys survive the GPU path");
 ColorReadback colour(gpu.device.Get(),FlowGpu::width,FlowGpu::height),originalColour(gpu.device.Get(),FlowGpu::width,FlowGpu::height);
 colour.read(gpu.queue.Get(),after2.Get());originalColour.read(gpu.queue.Get(),before2.Get());
 colour.verify(FlowGpu::width,FlowGpu::height,2,false);originalColour.verify(FlowGpu::width,FlowGpu::height,2,true);
 void* bytes=nullptr;check(motionReadback->Map(0,nullptr,&bytes),"motion readback map");
 {struct Unmap {ID3D12Resource* resource;~Unmap(){D3D12_RANGE empty{};resource->Unmap(0,&empty);}}unmap{motionReadback.Get()};
  for(uint32_t y=0;y<FlowGpu::guideHeight;++y)for(uint32_t x=0;x<FlowGpu::guideWidth;++x){
   uint32_t actual=0;
   std::memcpy(&actual,static_cast<const std::byte*>(bytes)+static_cast<size_t>(y)*motionLayout.Footprint.RowPitch+x*4,4);
   expect(actual==mv_pixel(2,x,y),"exact shared motion texel");
  }
 }
 expect(consumer.close()==Status::Closed,"flow consumer releases imports");
 expect(producer.retire()==Status::Ok,"flow producer retires");
 std::puts("PASS fg_ipc_gpu_flow colour=1920x1080 split=true guide=1344x756 mv_texels_exact=true pair_gated=true");
}
void reject_first_import(Renderer& renderer,Producer& producer){
 // Given the child's first import is injected to fail, When its bundle is claimed,
 // Then the transport reports an explicit rejection and retires without a second
 // close of any already-consumed handle.
 expect(SetEnvironmentVariableW(L"OBS_NR_TEST_REJECT_FIRST_IMPORT",L"1")!=FALSE,"inject first import rejection");
 Child process(Mode::Rejected,0);const auto offer=producer.connect(process.owned);process.offer(offer,renderer.adapter);
 await(producer.control_event(),process.process.value);finish(process.process.value);
 expect(producer.status()==Status::Rejected||producer.status()==Status::PeerDead,
  "injected import rejection is an explicit quarantine, not a silently accepted bundle");
 expect(producer.retire()==Status::Ok,"rejected import bundle retires without retained custody");
 expect(Producer::retained_generations()==0,"rejected import bundle keeps no quarantined generation");
 SetEnvironmentVariableW(L"OBS_NR_TEST_REJECT_FIRST_IMPORT",nullptr);
 std::puts("PASS fg_ipc_reject_first_import status=Rejected double_close=0");
}
int child(HANDLE control){
 View view;view.map(control,sizeof(Bootstrap));auto& b=*view.get<Bootstrap>();await(b.start,b.parent);
 Reader reader(b.adapter);const Offer offer{b.mapping,b.generation};const Peer parent{b.parent,b.parentPid,b.parentCreation};
 try{
  Consumer consumer(reader.queue.Get(),offer,parent);
  switch(b.mode){
  case Mode::Frames:break;
  case Mode::Crash:consumer.test_submit_probe(b.reached,b.resume);break;
  case Mode::Rejected:throw std::runtime_error("injected first import was not rejected");
  case Mode::Version:case Mode::Adapter:throw std::runtime_error("invalid header connection unexpectedly admitted");
  }
  await(b.go,b.parent);
  uint64_t expected=1;Handle done;done.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);expect(done.value!=nullptr,"one-use completion event");
  while(expected<=b.frames){
   std::optional<Frame> next;size_t index=0;
   for(size_t i=0;i<slot_count;++i){const auto f=consumer.frame(i);if(f&&(!next||f->serial<next->serial)){next=f;index=i;}}
   if(!next){await(consumer.frame_event(),b.parent);continue;}
   const auto wireSerial=expected+(b.frames==continuous_frames&&expected>=first_frame_after_switch?1:0);
   expect(next->serial==wireSerial&&next->generation==b.generation&&next->timestampNs==expected*1000&&next->arrivalNs==expected*1000+7,"wire serial skips once after an effect switch, keeping frame identity and timestamps");
   expect(next->shape.width==16&&next->shape.height==16&&next->shape.format==shape().format&&next->shape.colorSpace==shape().colorSpace&&
    next->shape.displayCapture&&next->shape.sourceMonitor==shape().sourceMonitor,"no torn shape/provenance");
   expect(consumer.arm_return(index,done.value)==Status::Ok,"subscribe exact return before GPU submission");
   expect(consumer.copy(index,reader.buffer.Get())==Status::Ok,"D3D12 consume");
   await(done.value,b.parent);reader.checksum(expected);expect(ReleaseSemaphore(b.step,1,nullptr)!=FALSE,"count frame checksum completion");++expected;
  }
  expect(consumer.close()==Status::Closed,"clean imports released after exact completion");return 0;
 }catch(const Error& error){
  switch(b.mode){
  case Mode::Version:expect(error.status()==Status::VersionMismatch,"strict version rejection reason");return 0;
  case Mode::Adapter:expect(error.status()==Status::AdapterMismatch,"strict LUID rejection reason");return 0;
  case Mode::Rejected:expect(error.status()==Status::Rejected,"injected first import rejection reason");return 0;
  case Mode::Frames:case Mode::Crash:throw;
  }
  throw;
 }
}
Header header_fixture(){
 Header h{};h.magic=0x314350494746524e;h.version=protocol_version;h.bytes=sizeof(Header);h.generation=1;h.luid={7,2};
 return h;
}
void version_when_incompatible(){
 // Given a pre-v3 or a future version, When the wire header is parsed, Then both
 // are rejected at the ABI boundary.
 auto h=header_fixture();
 h.version=2;
 expect(validate(h,{7,2})==Status::VersionMismatch,"legacy v2 header");
 h.version=protocol_version+1;
 expect(validate(h,{7,2})==Status::VersionMismatch,"future header");
 auto truncated=header_fixture();truncated.bytes=sizeof(Header)-8;
 expect(validate(truncated,{7,2})==Status::VersionMismatch,"truncated header");
}
void adapter_when_incompatible(){
 // Given a different LUID, When the wire header is parsed, Then it is rejected.
 const auto h=header_fixture();
 const auto result=validate(h,{8,2});
 expect(result==Status::AdapterMismatch,"LUID boundary");
}
void snapshot_when_interrupted(){
 // Given an interrupted write, When sampled, Then no torn image escapes.
 auto h=header_fixture();auto& f=h.frames[0];InterlockedExchange64(&f.sequence,3);InterlockedExchange64(&f.words[1],2);
 const auto result=snapshot(f);
 expect(!result,"odd interrupted writer is bounded pending");
}
void snapshot_when_committed(){
 // Given a committed frame, When sampled, Then its actual fields survive parsing.
 auto h=header_fixture();auto& f=h.frames[0];InterlockedExchange64(&f.words[1],2);
 InterlockedExchange64(&f.words[0],1);InterlockedExchange64(&f.words[2],2000);InterlockedExchange64(&f.words[3],2007);
 InterlockedExchange64(&f.words[4],(LONG64{16}<<32)|16);
 InterlockedExchange64(&f.words[15],16666667);
 InterlockedExchange64(&f.words[5],static_cast<LONG64>(DXGI_FORMAT_R8G8B8A8_UNORM));InterlockedExchange64(&f.sequence,4);
 const auto result=snapshot(f);
 expect(result&&result->serial==2&&result->timestampNs==2000&&result->shape.width==16&&result->obsTick==16666667&&
  !result->flow.hasSharedMv&&result->shape.guides.width==0,"committed wire value and OBS correlation tick");
}
WireFrame flow_fixture(uint64_t serial){
 // 1920x1080 colour with an independent 1344x756 analysis extent: the colour
 // height is not the guide height even in split mode.
 WireFrame f{};
 InterlockedExchange64(&f.words[0],7);InterlockedExchange64(&f.words[1],static_cast<LONG64>(serial));
 InterlockedExchange64(&f.words[2],static_cast<LONG64>(serial*1000));InterlockedExchange64(&f.words[3],static_cast<LONG64>(serial*1000+7));
 InterlockedExchange64(&f.words[4],(LONG64{1080}<<32)|1920);
 InterlockedExchange64(&f.words[5],static_cast<LONG64>(DXGI_FORMAT_R8G8B8A8_UNORM));
 InterlockedExchange64(&f.words[16],1);InterlockedExchange64(&f.words[17],1);
 InterlockedExchange64(&f.words[18],(LONG64{756}<<32)|1344);
 InterlockedExchange64(&f.words[19],(LONG64{0x3f800000}<<32)|0x3fc00000);
 InterlockedExchange64(&f.words[20],41);InterlockedExchange64(&f.words[21],42);
 InterlockedExchange64(&f.words[22],4);
 const auto key_at=[&](size_t at,uint64_t keySerial){
  InterlockedExchange64(&f.words[at],(LONG64{7}<<32)|0x51);
  InterlockedExchange64(&f.words[at+1],static_cast<LONG64>(keySerial));
  InterlockedExchange64(&f.words[at+2],static_cast<LONG64>(keySerial*16666667));
  InterlockedExchange64(&f.words[at+3],1);
  InterlockedExchange64(&f.words[at+4],2);
 };
 key_at(key_word,serial);
 key_at(prev_key_word,serial>1?serial-1:40);
 key_at(curr_key_word,serial);
 InterlockedExchange64(&f.sequence,static_cast<LONG64>(serial*2-1));
 InterlockedExchange64(&f.sequence,static_cast<LONG64>(serial*2));
 return f;
}
void flow_snapshot_bounds(){
 // Given an independent guide extent and displayed colour keys, When parsed,
 // Then the extents and keys survive and every contradictory record is rejected.
 const auto ok=snapshot(flow_fixture(1));
 expect(ok&&ok->shape.width==1920&&ok->shape.height==1080&&ok->shape.guides.width==1344&&
  ok->shape.guides.height==756&&ok->flow.hasSharedMv&&ok->flow.owner==FlowOwner::NrOwned&&
  ok->flow.provenance==FlowProvenance::Valid&&ok->flow.mvScaleX==1.5f&&ok->flow.mvScaleY==1.0f&&
  ok->flow.pairPrevTick==41&&ok->flow.pairCurrTick==42&&ok->key.submissionSerial==1&&
  ok->flow.currColor==ok->key&&ok->flow.prevColor.submissionSerial==40,
  "guide extent and displayed colour keys survive the wire");
 const auto reject=[&](void(*mutate)(WireFrame&),const char* why){
  auto bad=flow_fixture(2);mutate(bad);expect(!snapshot(bad),why);
 };
 reject([](WireFrame& w){InterlockedExchange64(&w.words[18],1344);},"motion frame without a guide height");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[16],3);},"unknown flow owner");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[17],6);},"unknown flow provenance");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[21],0);},"motion frame without a current pair tick");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[22],8);},"undefined pair flag bit");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[23],1);},"nonzero reserved word");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[19],0x3f800000);},"zero motion scale");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[16],0);},"unavailable owner with motion");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[25],0);},"motion frame without a colour key");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[30],0);},"motion frame without a guide previous key");
 reject([](WireFrame& w){InterlockedExchange64(&w.words[35],9);},"guide current endpoint keyed to another colour slot");
 reject([](WireFrame& w){for(size_t i=0;i<5;++i)
  InterlockedExchange64(&w.words[prev_key_word+i],InterlockedCompareExchange64(&w.words[curr_key_word+i],0,0));},
  "identical guide endpoints cannot bind a pair");
 std::puts("PASS fg_ipc_cpu_flow guide_extent_independent=true displayed_keys_bound=true malformed_flow_rejected=11");
}
struct CloseSourceFailure {
 std::array<HANDLE,3> sources{};
 std::array<unsigned,3> attempts{};
 bool validRequest=true,replacementOpen=true;
} closeSourceFailure;
BOOL WINAPI close_source_failure(HANDLE,HANDLE source,HANDLE,LPHANDLE output,DWORD,BOOL,DWORD options){
 auto& probe=closeSourceFailure;const auto found=std::find(probe.sources.begin(),probe.sources.end(),source);
 if(found==probe.sources.end()||(options&DUPLICATE_CLOSE_SOURCE)==0)probe.validRequest=false;
 else{
  const auto index=static_cast<size_t>(found-probe.sources.begin());
  // Every call consumes the source, despite FALSE. The first number immediately
  // denotes an unrelated replacement after its original source was consumed.
  if(index==0&&probe.attempts[index]>0)probe.replacementOpen=false;
  ++probe.attempts[index];
 }
 *output=nullptr;SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FALSE;
}
void revocation_when_close_source_fails(){
 // Given close-on-failure and deterministic replacement under a consumed number,
 // When the real revoke loop runs twice, Then replacement and remaining custody survive correctly.
 closeSourceFailure={};closeSourceFailure.sources={reinterpret_cast<HANDLE>(uintptr_t{0x40}),
  reinterpret_cast<HANDLE>(uintptr_t{0x44}),reinterpret_cast<HANDLE>(uintptr_t{0x48})};
 std::array<HANDLE,handle_count> remote{};remote[0]=closeSourceFailure.sources[0];remote[1]=closeSourceFailure.sources[1];remote[17]=closeSourceFailure.sources[2];
 const auto result=Producer::test_revoke(remote,&close_source_failure);
 expect(closeSourceFailure.validRequest&&closeSourceFailure.replacementOpen,"failed CLOSE_SOURCE must never retry an unrelated replacement");
 expect(closeSourceFailure.attempts==std::array<unsigned,3>{1,1,1},"all original bundle handles must be consumed once, including after failure");
 expect(std::all_of(remote.begin(),remote.end(),[](HANDLE h){return h==nullptr;}),"failed close-source attempts must not retain numeric custody");
 expect(result.first&&result.second&&result.error==ERROR_NOT_ENOUGH_MEMORY,"API failure stays diagnostic, not continued source ownership");
}
void reject(Renderer& renderer,Producer& producer,Mode mode){
 // Given a real duplicated bundle, When its immutable header is incompatible,
 // Then the child rejects before claiming handles, and producer revokes safely.
 Child process(mode,0);const auto offer=producer.connect(process.owned);
 Handle mapping;mapping.value=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,offer.mapping.c_str());expect(mapping.value!=nullptr,"negative offer mapping");
 View view;view.map(mapping.value,sizeof(Header));auto& h=*view.get<Header>();
 switch(mode){case Mode::Version:h.version=2;break;case Mode::Adapter:h.luid.LowPart^=1;break;
 case Mode::Frames:case Mode::Crash:case Mode::Rejected:throw std::runtime_error("invalid rejection test mode");}
 process.offer(offer,renderer.adapter);finish(process.process.value);expect(producer.retire()==Status::Ok,"unclaimed negative bundle retirement");
}
void frames(Renderer& renderer,Producer& producer){
 // Given a ready child deliberately held before acquisition, When four frames
 // fill the ring, Then a fifth is dropped and all four original pixels survive.
 Child process(Mode::Frames,continuous_frames);const auto offer=producer.connect(process.owned);process.offer(offer,renderer.adapter);
 await(producer.control_event(),process.process.value);expect(producer.status()==Status::Ok,"import handshake ready");
 for(uint64_t n=1;n<=4;++n)renderer.send(producer,n);
 expect(producer.publish(renderer.source.Get(),5000,5007)==Status::Dropped,"held four slots refuse overwrite");
 signal(process.go.value);for(unsigned n=0;n<4;++n)await(process.step.value,process.process.value);
 for(uint64_t n=first_frame_after_switch;n<=continuous_frames;++n){renderer.send(producer,n,n==first_frame_after_switch);await(process.step.value,process.process.value);}
 finish(process.process.value);expect(producer.retire()==Status::Ok,"1000-frame clean retirement");
 std::puts("PASS fg_ipc_gpu frames=1000 checksum_mismatch=0 held_slot_overwrite=0 torn_header=0");
}
uint64_t crash(Renderer& renderer,Producer& producer){
 // Given an exact post-Execute/pre-Signal checkpoint, When only this owned child
 // is killed, Then process detection is bounded and the old return is NOT invented.
 Child process(Mode::Crash,1);const auto offer=producer.connect(process.owned);process.offer(offer,renderer.adapter);
 await(producer.control_event(),process.process.value);expect(producer.status()==Status::Ok,"crash importer handshake");
 renderer.send(producer,1);signal(process.go.value);await(process.reached.value,process.process.value);
 const auto latency=process.kill();expect(producer.status()==Status::PeerDead,"retained process HANDLE detects death");
 expect(producer.retire()==Status::Ok,"quarantine uncertain submitted generation");
 std::printf("PASS fg_ipc_owned_kill detection_ms=%.3f checkpoint=post_execute_pre_signal\n",latency);return offer.generation;
}
void gpu_contract(){
 Renderer renderer;const auto self=identity(GetCurrentProcess());
 const auto name=L"Local\\nr-fg-ipc-test-"+std::to_wstring(self.pid)+L"-"+std::to_wstring(self.creation);
 Producer producer(renderer.device.Get(),name,shape());
 reject(renderer,producer,Mode::Version);reject(renderer,producer,Mode::Adapter);reject_first_import(renderer,producer);frames(renderer,producer);
 const auto old=crash(renderer,producer);
 // Given one quarantined generation, When a fresh child connects, Then it has a
 // greater generation and a working independent resource set, without GPU idle.
 Child replacement(Mode::Frames,1);const auto fresh=producer.connect(replacement.owned);expect(fresh.generation>old,"reconnect generation advances");
 replacement.offer(fresh,renderer.adapter);await(producer.control_event(),replacement.process.value);
 expect(producer.status()==Status::Ok,"reconnect handshake");renderer.send(producer,1);signal(replacement.go.value);
 await(replacement.step.value,replacement.process.value);finish(replacement.process.value);expect(producer.retire()==Status::Ok,"replacement retires cleanly");
 // Given two crashes with no possible return Signal, When reconnect is requested,
 // Then no third physical generation is allocated, including across object lifetime.
 crash(renderer,producer);expect(Producer::retained_generations()==generation_limit,"bounded crash quarantine count");
 Child refused(Mode::Frames,0);bool capacity=false;
 try{producer.connect(refused.owned);}catch(const Error& error){capacity=error.status()==Status::RetirementCapacity;}
 expect(capacity&&Producer::retained_generations()==generation_limit,"typed capacity preserves bounded generations");
 {Producer another(renderer.device.Get(),name+L"-replacement-owner",shape());bool refusedAgain=false;
  try{another.connect(refused.owned);}catch(const Error& error){refusedAgain=error.status()==Status::RetirementCapacity;}
  expect(refusedAgain,"recreating producer does not evade retirement bound");}
 std::puts("PASS fg_ipc_gpu_reconnect version_reject=true luid_reject=true bounded_generations=2");
}
}
int wmain(int argc,wchar_t** argv){
 try{
  if(argc==3&&std::wcscmp(argv[1],L"--child")==0){
   wchar_t* end=nullptr;const auto value=std::wcstoull(argv[2],&end,10);
   expect(end&&*end==L'\0'&&value!=0,"child control HANDLE argument");return child(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)));
  }
  if(argc==2&&std::wcscmp(argv[1],L"--cpu")==0){
   version_when_incompatible();adapter_when_incompatible();snapshot_when_interrupted();snapshot_when_committed();
   flow_snapshot_bounds();revocation_when_close_source_fails();
   std::puts("PASS fg_ipc_cpu");return 0;
  }
  if(argc==2&&std::wcscmp(argv[1],L"--gpu")==0){flow_contract();gpu_contract();return 0;}
  throw std::runtime_error("expected --cpu or explicitly admitted --gpu");
 }catch(const std::exception& error){std::fprintf(stderr,"FAIL fg_ipc: %s\n",error.what());return 1;}
}
