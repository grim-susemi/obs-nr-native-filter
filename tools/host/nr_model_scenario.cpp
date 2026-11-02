// SPDX-License-Identifier: GPL-3.0-only
#include "nr_model_scenario.h"
#include "nr_model.h"
#include "nr_model_ownership.h"
#include "capture_pin_observation.h"
#include "nr_color_scenario.h"
#include <obs.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <DirectXPackedVector.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
using namespace nr;
using Microsoft::WRL::ComPtr;
constexpr uint32_t width=1920,height=1080;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void checked(HRESULT value,const char* message) { require(SUCCEEDED(value),message); }
struct Event {
    HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Event() { require(value!=nullptr,"native QA event"); }
    ~Event() { CloseHandle(value); }
    void wait() { require(WaitForSingleObject(value,30000)==WAIT_OBJECT_0,"native QA exact event timeout"); }
};
std::vector<std::byte> bytes(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate); require(static_cast<bool>(file),"native shader file");
    const auto size=file.tellg(); require(size>0,"native shader bytes");
    std::vector<std::byte> result(static_cast<size_t>(size)); file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()),size); require(static_cast<bool>(file),"native shader read");
    return result;
}
uint64_t hash(std::span<const uint8_t> data) {
    uint64_t result=14695981039346656037ull; for(auto byte:data) result=(result^byte)*1099511628211ull; return result;
}
std::vector<uint8_t> pattern(uint64_t sequence) {
    std::vector<uint8_t> result(static_cast<size_t>(width)*height*4);
    for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
        const uint32_t shifted=x+static_cast<uint32_t>(sequence);
        uint32_t noise=shifted*0x9e3779b9u^y*0x85ebca6bu; noise^=noise>>16;
        const uint32_t tile=((shifted/32)^(y/32))&1u;
        const auto at=(static_cast<size_t>(y)*width+x)*4;
        result[at]=static_cast<uint8_t>(32+tile*80+(noise&31));
        result[at+1]=static_cast<uint8_t>(64+tile*80+((noise>>5)&31));
        result[at+2]=static_cast<uint8_t>(80+tile*80+((noise>>10)&31));
        result[at+3]=255;
    }
    return result;
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&barrier);
}
// Exact owner objects of one readback submission. Once the command list has been executed
// these must outlive this function if completion was not exactly confirmed.
struct ReadbackOwner {
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    Event complete;
};
// Finite QA only, outside renderer and after exact model physical completion.
std::vector<uint8_t> readback(interop::Context& context,ID3D12Resource* resource) {
    require(resource!=nullptr,"native GPU readback resource");
    const auto desc=resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 total=0;
    context.device()->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);
    auto owner=std::make_unique<ReadbackOwner>();
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=total;
    buffer.Height=1; buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1;
    buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    checked(context.device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&owner->target)),"native QA readback allocation");
    checked(context.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&owner->allocator)),"native QA allocator");
    checked(context.device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,owner->allocator.Get(),nullptr,
        IID_PPV_ARGS(&owner->commands)),"native QA list");
    checked(context.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&owner->fence)),"native QA fence");
    transition(owner->commands.Get(),resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=owner->target.Get();
    destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint=footprint;
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=resource; source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    owner->commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    transition(owner->commands.Get(),resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    checked(owner->commands->Close(),"native QA close");
    checked(owner->fence->SetEventOnCompletion(1,owner->complete.value),"native QA pre-trigger completion subscription");
    ID3D12CommandList* lists[]{owner->commands.Get()}; context.queue()->ExecuteCommandLists(1,lists);
    // From here the GPU owns the submission. Signal failure and an unobserved completion
    // event are both unresolved: the exact owner objects move to process-lifetime custody
    // and the caller must not run the settled cleanup.
    const auto signal=context.queue()->Signal(owner->fence.Get(),1);
    const auto observed=SUCCEEDED(signal) && WaitForSingleObject(owner->complete.value,30000)==WAIT_OBJECT_0?
        owner->fence->GetCompletedValue():0;
    if(qa::settle_readback_owner(true,signal,observed,1,owner)) {
        std::cout<<"NR_MODEL_READBACK_RETAINED unresolved=true signal="<<static_cast<uint32_t>(signal)
            <<" observed="<<observed<<" required=1 pid="<<GetCurrentProcessId()<<std::endl;
        throw std::runtime_error("native QA unresolved readback completion");
    }
    // The helper only declines custody on an exactly confirmed completion; keep the explicit
    // check so a future helper change can never silently proceed without the required value.
    require(observed==1,"native QA physical completion");
    const size_t pixelBytes=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;    const size_t row=static_cast<size_t>(desc.Width)*pixelBytes;
    std::vector<uint8_t> result(row*desc.Height); void* mapped=nullptr;
    const D3D12_RANGE range{0,static_cast<SIZE_T>(total)}; checked(owner->target->Map(0,&range,&mapped),"native QA completed map");
    for(UINT y=0;y<desc.Height;++y) memcpy(result.data()+y*row,static_cast<const uint8_t*>(mapped)+y*footprint.Footprint.RowPitch,row);
    const D3D12_RANGE empty{0,0}; owner->target->Unmap(0,&empty); return result;
}
struct Snapshot {
    ComPtr<ID3D11Texture2D> original11,analysis11;
    ComPtr<ID3D12Resource> original12,analysis12;
};
enum class Action { None,Init,Capture,Cut,Stop,Close };
struct Sample {
    const JobContract contract;
    const interop::Ticket ticket;
    std::optional<interop::Consumer> pin;
    const qa::CapturePinObservation pinObservation;
    std::shared_ptr<model::Job> job;
    std::shared_future<void> returned;
    const uint64_t clock,interval;
    const uint64_t expected;
};
struct Harness {
    // All cleanup and action events exist before native device/model submissions.
    Event acted;
    std::shared_ptr<interop::ConsumerEvent> consumerComplete;
    std::shared_ptr<const interop::ConsumerEnd> consumerEnd;
    std::atomic<Action> action{Action::None};
    bool failed=false;
    std::string error;
    model::Config config;
    std::vector<std::byte> vertex,pixel;
    std::vector<uint8_t> pixels;
    uint64_t expectedHash=0;
    model::Fault fault;
    const uint64_t instance;
    gs_texture_t* source=nullptr;
    ComPtr<ID3D11Device> renderer;
    DXGI_ADAPTER_DESC1 adapterDescription{};
    ComPtr<ID3D11DeviceContext> graphics;
    ComPtr<ID3D11Texture2D> target11;
    ComPtr<ID3D12Resource> target12;
    std::shared_ptr<interop::Context> context;
    std::shared_ptr<interop::Bridge> bridge;
    std::shared_ptr<model::Processor> processor;
    std::unique_ptr<pipeline::FramePipeline> pipeline;
    std::vector<std::unique_ptr<Sample>> samples;
    std::optional<pipeline::Admission> admission;
    std::shared_future<void> exited;
    uint64_t sequence=0,consumerValue=0,selectedSequence=0;
    OutputKind selected=OutputKind::CurrentSource;
    uint64_t nrSelections=0,rawSelections=0;
    uint64_t eligibleJobs=0;
    bool stopping=false;
    Harness(const std::filesystem::path& root,model::Fault control,unsigned lane):fault(control),instance(111+lane) {
        config.runtime={std::filesystem::canonical(L"C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll"),
            "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a"};
        config.driver=runtime::resolve_driver().identity.canonicalPath;
        config.data=std::filesystem::canonical(root/"profile/ngx-data");
        config.codec=bytes(root/"bin/64bit/nr_codec.dxil"); config.color=bytes(root/"bin/64bit/nr_color.dxil");
        config.guides=bytes(root/"bin/64bit/nr_guides.cso");
        vertex=bytes(root/"bin/64bit/nr_interop_vertex.cso"); pixel=bytes(root/"bin/64bit/nr_interop_pixel.cso");
    }
    void execute(Action next) {
        ResetEvent(acted.value); action.store(next,std::memory_order_release); acted.wait();
        require(!failed,error.c_str());
    }
    std::shared_ptr<Snapshot> snapshot(ID3D11Texture2D* borrowed) {
        auto result=std::make_shared<Snapshot>();
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        auto copy=[&](ComPtr<ID3D11Texture2D>& texture,ComPtr<ID3D12Resource>& opened) {
            checked(renderer->CreateTexture2D(&desc,nullptr,&texture),"native immutable OBS snapshot");
            ComPtr<IDXGIResource1> shared; checked(texture.As(&shared),"native snapshot NT handle interface");
            HANDLE handle=nullptr;
            checked(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),
                "native snapshot shared handle");
            const auto hr=interop::open_shared_resource(context->device(),handle,&opened);
            CloseHandle(handle); checked(hr,"native snapshot private open");
            graphics->CopyResource(texture.Get(),borrowed);
        };
        copy(result->original11,result->original12); copy(result->analysis11,result->analysis12);
        return result;
    }
    void init() {
        require(gs_get_device_type()==GS_DEVICE_DIRECT3D_11,"native actual OBS D3D11 renderer");
        source=gs_texture_create(width,height,GS_BGRA,1,nullptr,GS_DYNAMIC); require(source!=nullptr,"native OBS fixture texture");
        auto* borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(source)); require(borrowed!=nullptr,"native OBS texture object");
        borrowed->GetDevice(&renderer); renderer->GetImmediateContext(&graphics);
        context=interop::Context::create(renderer.Get());
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 desc{};
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"native factory");
        checked(factory->EnumAdapterByLuid(context->luid(),IID_PPV_ARGS(&adapter)),"native same LUID");
        checked(adapter->GetDesc1(&desc),"native physical adapter");
        adapterDescription=desc;
        require(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) &&
            std::wstring(desc.Description).find(L"RTX 4090")!=std::wstring::npos,"native physical RTX4090 required");
        require(interop::same_luid(context->luid(),context->device()->GetAdapterLuid()),"native private worker same LUID");
        const FrameDescriptor descriptor{{width,height},{0,0,width,height},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80};
        bridge=std::make_shared<interop::Bridge>(context,1,descriptor,vertex,pixel);
        processor=std::make_shared<model::Processor>(context,config,instance);
        pipeline=std::make_unique<pipeline::FramePipeline>(Epochs{instance,1,1,1},processor);
        require(pipeline->hold_generation(1,processor),"native physical feature generation hold");
        D3D11_TEXTURE2D_DESC target{}; target.Width=width; target.Height=height;
        target.MipLevels=target.ArraySize=target.SampleDesc.Count=1; target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        target.Usage=D3D11_USAGE_DEFAULT; target.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        target.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        checked(renderer->CreateTexture2D(&target,nullptr,&target11),"native output consumer target");
        ComPtr<IDXGIResource1> shared; checked(target11.As(&shared),"native output shared interface"); HANDLE handle=nullptr;
        checked(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),"native output handle");
        const auto hr=interop::open_shared_resource(context->device(),handle,&target12); CloseHandle(handle); checked(hr,"native output private open");
    }
    void observe() {
        for(const auto& sample:samples) {
            if(!sample->job || !sample->job->ready.load(std::memory_order_acquire)) continue;
            const auto physical=bridge->ownership(sample->ticket.slot);
            if(physical.key && *physical.key==sample->ticket.key) {
                if(sample->job->composed && !sample->job->eligible && sample->job->submitted) {
                    require(pipeline->cancel(sample->contract.key) && bridge->cancel(sample->ticket),"native invalid publication cancellation");
                    require(pipeline->observe(sample->ticket,bridge->ownership(sample->ticket.slot)),"native actual cancelled ownership receipt");
                } else require(pipeline->observe(sample->ticket,physical),"native exact physical observer");
            }
        }
    }
    void end_pin(Sample& sample,bool notify) {
        if(sample.pin) {
            auto event=notify?interop::ConsumerEvent::create():nullptr;
            require(!notify || event,"native fresh raw pin completion event");
            const auto ending=bridge->end_consumer(*sample.pin,event);
            require(static_cast<bool>(ending),"native captured raw pin end");
            consumerValue=ending->completionValue;
            if(notify){consumerComplete=event;consumerEnd=ending;}
            sample.pin.reset();
        }
    }
    void capture() {
        observe();
        const auto clock=obs_get_video_frame_time(),interval=obs_get_frame_interval_ns();
        require(clock && interval,"native actual OBS clock");
        const auto tick=clock/interval+1;
        const auto epochs=pipeline->epochs(); ++sequence;
        const Settings settings{{1},true,config.runtime.canonicalPath.generic_string(),{1},core_default_tuning()};
        const IngressSample input{{instance,epochs.contentEpoch,sequence},tick,tick-1,0x111a+instance,reinterpret_cast<uintptr_t>(renderer.Get()),
            epochs.resourceGeneration,SourceValidity::Valid,
            {{width,height},{0,0,width,height},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},settings};
        const auto plan=pipeline->begin(input,tick+1); require(plan.admission.has_value(),"native scheduler admission");
        admission.emplace(*plan.admission);
        gs_texture_set_image(source,pixels.data(),width*4,false);
        auto* borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(source));
        auto immutable=snapshot(borrowed);
        const auto ticket=bridge->capture(admission->job.key,borrowed);
        require(ticket && ticket->slot==admission->slot,"native exact Bridge ticket == scheduler slot");
        selected=plan.selection.kind; selectedSequence=plan.selection.frame?plan.selection.frame->ingressSequence:0;
        consumerComplete.reset();consumerEnd.reset();consumerValue=0;
        const bool delayed=selected==OutputKind::DelayedNr || selected==OutputKind::DelayedRaw;
        for(const auto& old:samples) if(old->pin) end_pin(*old,!delayed);
        if((selected==OutputKind::DelayedNr || selected==OutputKind::DelayedRaw) && !samples.empty()) {
            auto& old=*samples.back();
            const auto consumer=selected==OutputKind::DelayedNr?bridge->acquire_output(old.ticket):bridge->acquire_raw(old.ticket);
            require(consumer.has_value(),"native actual delayed consumer acquire");
            if(selected==OutputKind::DelayedNr) {
                require(bridge->copy_to(*consumer,target11.Get()),"native output consumer copy"); ++nrSelections;
            } else {
                auto original=std::static_pointer_cast<Snapshot>(old.job->frame->graphicsLease);
                graphics->CopyResource(target11.Get(),original->original11.Get()); ++rawSelections;
            }
            consumerComplete=interop::ConsumerEvent::create();
            require(static_cast<bool>(consumerComplete),"native fresh selected consumer event");
            consumerEnd=bridge->end_consumer(*consumer,consumerComplete);
            require(static_cast<bool>(consumerEnd),"native actual selected consumer end");
            if(selected==OutputKind::DelayedNr)
                require(processor->publish(old.job,plan.selection,bridge->ownership(old.ticket.slot)),"native deadline-latched applied publication");
            consumerValue=consumerEnd->completionValue;
        }
        // Capture has bound the current image, but no worker can discard/reuse it
        // before finish. Pin identity is independent of final ending order.
        const auto pin=bridge->acquire_raw(*ticket); require(pin.has_value(),"native capture pin");
        const auto physical=bridge->ownership(ticket->slot); require(physical.consumerOpen,"native captured pin ownership");
        const auto pinObservation=qa::CapturePinObservation::observe(admission->job.key,*ticket,*pin,
            *bridge,context->graphics_thread(),physical);
        require(pinObservation.has_value(),"native graphics-owner exact capture pin observation");
        auto frame=std::make_shared<model::Frame>(immutable->original12.Get(),immutable->analysis12.Get(),immutable,
            NV_OF_FENCE_POINT{bridge->producer_fence(),physical.producer});
        const bool controlled=fault!=model::Fault::None &&
            ((fault==model::Fault::Capability || fault==model::Fault::Creation ||
              fault==model::Fault::AfterCoreInit) || sequence>1);
        auto job=std::make_shared<model::Job>(frame,admission->job,ColorExposure{1,1},controlled?fault:model::Fault::None);
        auto raw=std::make_shared<const pipeline::RawFrame>(pipeline::RawFrame{input,frame});
        const pipeline::WorkBinding binding{*ticket,physical,bridge,job,pin};
        require(pipeline->finish(raw,&binding),"native scheduler dispatch finish");
        const auto* view=pipeline->view(tick);
        require(view && view->selection.canonicalTick==tick && view->selection.kind==selected,"native immutable same tick view");
        require(pipeline->view(tick)==view,"native all-view same phase latch");
        samples.push_back(std::make_unique<Sample>(Sample{admission->job,*ticket,pin,*pinObservation,job,admission->returned,clock,interval,expectedHash}));
    }
    void stop() {
        observe();consumerComplete.reset();consumerEnd.reset();consumerValue=0;
        for(const auto& sample:samples) if(sample->pin) end_pin(*sample,true);
        stopping=true;
    }
    void close() {
        require(stopping,"native stop receipt");
        if(!exited.valid()) {
            observe();
            for(const auto& sample:samples) if(pipeline->state(sample->contract.key))
                require(pipeline->cancel(sample->contract.key),"native irreversible cancel");
            const auto epochs=pipeline->epochs();
            const auto tick=obs_get_video_frame_time()/obs_get_frame_interval_ns()+1;
            const IngressSample disabled{{instance,epochs.contentEpoch,++sequence},tick,tick-1,0x111a+instance,reinterpret_cast<uintptr_t>(renderer.Get()),
                epochs.resourceGeneration,SourceValidity::Valid,
                {{width,height},{0,0,width,height},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},
                {{1},false,config.runtime.canonicalPath.generic_string(),{1},core_default_tuning()}};
            (void)pipeline->begin(disabled,tick+1); require(pipeline->finish({}),"native disabled cleanup latch");
            pipeline->collect(); samples.clear(); admission.reset();
            exited=pipeline->worker_exit(); // Subscribe BEFORE stop.
            pipeline->request_stop(); return;
        }
        require(processor->closed(),"native worker cleanup retained");
        require(pipeline->complete_generation(1),"native exact feature retirement");
        pipeline->collect(); require(pipeline->try_close(),"native exact scheduler close");
        pipeline.reset(); require(bridge->close(),"native exact Bridge physical close"); bridge.reset();
        processor.reset(); target12.Reset(); target11.Reset();
        gs_texture_destroy(source); source=nullptr; context.reset(); graphics.Reset(); renderer.Reset();
    }
};
void draw(void* opaque,uint32_t,uint32_t) {
    auto& h=*static_cast<Harness*>(opaque);
    const auto action=h.action.exchange(Action::None,std::memory_order_acquire); if(action==Action::None) return;
    try {
        if(action==Action::Init) h.init();
        else if(action==Action::Capture) h.capture();
        else if(action==Action::Cut) require(h.pipeline->reset(ResetCause::Cut),"native NR/OFA explicit cut reset");
        else if(action==Action::Stop) h.stop();
        else if(action==Action::Close) h.close();
    } catch(const std::exception& error) { h.failed=true; h.error=error.what(); }
    SetEvent(h.acted.value); // No wait, readback, foreign model call or logging on renderer.
}
void proof(Harness& h,unsigned lane,unsigned step) {
    auto& sample=*h.samples.back(); auto& job=*sample.job;
    require(sample.returned.wait_for(std::chrono::seconds(30))==std::future_status::ready,"native exact dispatch-return receipt");
    if(!job.diagnostic.empty()) {
        std::cout<<"NR_MODEL_DIAGNOSTIC lane="<<lane<<" step="<<step<<" epoch="<<job.epoch<<" code="<<job.code<<" "<<job.diagnostic<<std::endl;
        throw std::runtime_error(job.diagnostic);
    }
    require(WaitForSingleObject(job.observed,30000)==WAIT_OBJECT_0 && job.ready.load(std::memory_order_acquire),
        "native exact subscribed physical/status receipt");
    if(h.consumerValue) {
        require(h.consumerEnd && h.consumerEnd->event==h.consumerComplete &&
            WaitForSingleObject(h.consumerComplete->handle(),30000)==WAIT_OBJECT_0 &&
            interop::completion(h.consumerEnd->completionValue,h.consumerEnd->consumer.fence->GetCompletedValue())==
                interop::Completion::Complete,"native actual consumer complete");
    }
    // Admission refusals can return before the raw producer finishes. Preserve
    // the actual producer dependency for their finite QA readback too.
    checked(h.context->queue()->Wait(job.frame->producer.fence,job.frame->producer.value),"native QA raw producer dependency");
    const auto raw=readback(*h.context,job.frame->original.Get());
    const auto analysis=readback(*h.context,job.frame->analysis.Get());
    require(hash(raw)==sample.expected && hash(analysis)==sample.expected,"native untouched original/analysis GPU input binding");
    uint64_t inputHash=0,modelHash=0,motionHash=0,depthHash=0,outputHash=0; uint64_t changed=0;
    double motionX=0,motionY=0;
    if(job.eligible) {
        ++h.eligibleJobs;
        const auto colors=job.color_resources();
        const auto input=readback(*h.context,colors[1]),model=readback(*h.context,colors[3]);
        const auto motion=readback(*h.context,job.flow->motion),depth=readback(*h.context,job.flow->depth);
        const auto output=readback(*h.context,job.output);
        inputHash=hash(input); modelHash=hash(model); motionHash=hash(motion); depthHash=hash(depth); outputHash=hash(output);
        require(inputHash && modelHash && motionHash && depthHash && outputHash,"native actual GPU hashes");
        require(inputHash!=modelHash,"native Feature18 model output changed");
        for(size_t i=0;i<motion.size();i+=4) {
            uint16_t packed[2]{}; memcpy(packed,motion.data()+i,4);
            const float x=DirectX::PackedVector::XMConvertHalfToFloat(packed[0]);
            const float y=DirectX::PackedVector::XMConvertHalfToFloat(packed[1]);
            require(std::isfinite(x) && std::isfinite(y),"native finite actual RG16F guide pixels");
            motionX+=x; motionY+=y;
            float neutral=0; memcpy(&neutral,depth.data()+i,4); require(neutral==1,"native neutral depth is not geometry");
        }
        motionX/=static_cast<double>(width)*height; motionY/=static_cast<double>(width)*height;
        if(!job.flow->reset) require(motionX>0 && std::abs(motionX-1)<=1 && std::abs(motionY)<=1,
            "native current-to-previous guide sign/pixel units within accepted one-pixel bound");
        for(size_t i=0;i<raw.size();i+=4) {
            if(raw[i]!=output[i+2] || raw[i+1]!=output[i+1] || raw[i+2]!=output[i]) ++changed;
            require(raw[i+3]==output[i+3],"native actual output alpha");
        }
        require(changed!=0,"native actual source-relative changed output pixels");
        require(job.epoch>job.creationEpoch,"native explicit next submission epoch");
        for(size_t i=1;i<job.chronology.size()-1;++i) require(job.chronology[i] && job.chronology[i]<=job.chronology[i+1],
            "native chronological encode/evaluate/compose/submit/physical/status");
        const auto ids=job.descriptor_heaps(); require(ids[0] && ids[1] && ids[2] && ids[0]!=ids[1] && ids[1]!=ids[2],"native distinct descriptor heaps");
        for(size_t i=0;i<colors.size();++i) for(size_t j=0;j<i;++j) require(colors[i]!=colors[j],"native five distinct colors");
        require(job.flow->key==sample.contract.key && job.flow->depth->GetDesc().Format==DXGI_FORMAT_R32_FLOAT &&
            job.flow->motion->GetDesc().Format==DXGI_FORMAT_R16G16_FLOAT,"native exact guide key and effective formats");
    }
    uint64_t targetHash=0,targetChanged=0;
    if((h.selected==OutputKind::DelayedNr || h.selected==OutputKind::DelayedRaw) && h.samples.size()>1) {
        auto& selected=*h.samples[h.samples.size()-2];
        require(h.selectedSequence==selected.contract.key.frame.ingressSequence,"native exact n-1 selected key");
        const auto target=readback(*h.context,h.target12.Get());
        targetHash=hash(target);
        if(h.selected==OutputKind::DelayedRaw) require(targetHash==selected.expected,"native exact untouched n-1 raw");
        else {
            const auto oldOutput=readback(*h.context,selected.job->output);
            const auto oldRaw=readback(*h.context,selected.job->frame->original.Get());
            for(size_t i=0;i<target.size();i+=4) {
                require(target[i]==oldOutput[i+2] && target[i+1]==oldOutput[i+1] &&
                    target[i+2]==oldOutput[i] && target[i+3]==oldOutput[i+3],"native actual output Consumer pixel ownership");
                if(target[i]!=oldRaw[i] || target[i+1]!=oldRaw[i+1] || target[i+2]!=oldRaw[i+2]) ++targetChanged;
            }
            require(targetChanged,"native actual delayed NR Consumer changed pixels");
        }
    }
    const auto completion=job.completion();
    const auto captured=h.bridge->ownership(sample.ticket.slot);
    require(sample.pin && sample.pinObservation.retains(sample.contract.key,sample.ticket,*sample.pin,
        *h.bridge,h.context->graphics_thread(),captured),"native exact open capture pin retains ownership");
    std::cout<<"NR_MODEL_CASE {\"lane\":"<<lane<<",\"step\":"<<step<<",\"instance_id\":"<<sample.contract.key.frame.instanceId
        <<",\"content_epoch\":"<<sample.contract.key.frame.contentEpoch<<",\"history_epoch\":"<<sample.contract.key.historyEpoch
        <<",\"resource_generation\":"<<sample.contract.key.resourceGeneration<<",\"ingress_sequence\":"<<sample.contract.key.frame.ingressSequence
        <<",\"submission_serial\":"<<sample.contract.key.submissionSerial<<",\"settings_revision\":"<<sample.contract.input.settings.revision.value
        <<",\"source_identity\":"<<sample.contract.input.sourceIdentity<<",\"device_identity\":"<<sample.contract.input.deviceIdentity
        <<",\"clock\":"<<sample.clock<<",\"interval\":"<<sample.interval<<",\"tick\":"<<sample.contract.input.canonicalTick
        <<",\"deadline\":"<<sample.contract.deadlineTick<<",\"slot\":"<<sample.ticket.slot<<",\"epoch\":"<<job.epoch
        <<",\"creation_epoch\":"<<job.creationEpoch<<",\"created\":"<<(job.created?"true":"false")
        <<",\"evaluated\":"<<(job.evaluated?"true":"false")<<",\"composed\":"<<(job.composed?"true":"false")
        <<",\"submitted\":"<<(job.submitted?"true":"false")<<",\"gpu_composed\":"<<(job.eligible?"true":"false")
        <<",\"code\":"<<job.code<<",\"invalid_status\":"<<job.invalid<<",\"changed_pixels\":"<<changed
        <<",\"raw_hash\":\""<<hash(raw)<<"\",\"analysis_hash\":\""<<hash(analysis)<<"\",\"input_hash\":\""<<inputHash
        <<"\",\"model_hash\":\""<<modelHash<<"\",\"motion_hash\":\""<<motionHash<<"\",\"depth_hash\":\""<<depthHash
        <<"\",\"output_hash\":\""<<outputHash<<"\",\"feature\":18,\"width\":"<<width<<",\"height\":"<<height
        <<",\"control\":"<<static_cast<unsigned>(job.fault)<<",\"motion_mean_x\":"<<motionX<<",\"motion_mean_y\":"<<motionY
        <<",\"mv_scale_x\":1,\"mv_scale_y\":1,\"pre_exposure\":"<<job.exposure.preExposure<<",\"exposure\":"<<job.exposure.exposure
        <<",\"source_format\":"<<static_cast<unsigned>(job.frame->original->GetDesc().Format)
        <<",\"raw_resource\":\""<<reinterpret_cast<uintptr_t>(job.frame->original.Get())
        <<"\",\"analysis_resource\":\""<<reinterpret_cast<uintptr_t>(job.frame->analysis.Get())
        <<"\",\"output_resource\":\""<<reinterpret_cast<uintptr_t>(job.output)
        <<"\",\"status_resource\":\""<<reinterpret_cast<uintptr_t>(job.invalid_status_resource())<<"\""
        <<",\"reset\":"<<(job.flow && job.flow->reset?"true":"false")<<",\"selected\":"<<static_cast<unsigned>(h.selected)
        <<",\"selected_sequence\":"<<h.selectedSequence<<",\"target_hash\":\""<<targetHash
        <<"\",\"target_changed_pixels\":"<<targetChanged<<",\"producer\":"<<job.frame->producer.value
        <<",\"producer_done\":"<<job.frame->producer.fence->GetCompletedValue()
        <<",\"worker_done\":"<<h.bridge->worker_fence()->GetCompletedValue()<<",\"consumer\":"<<h.consumerValue
        <<",\"consumer_done\":"<<h.bridge->consumer_fence()->GetCompletedValue()
        <<",\"capture_pin\":"<<captured.consumerLease<<",\"capture_pin_open\":"<<(captured.consumerOpen?"true":"false")
        <<",\"physical_done\":"<<(completion.fence?completion.fence->GetCompletedValue():0)
        <<",\"module_base\":\""<<job.runtimeModule<<"\",\"driver_create_code\":"<<job.driverCreateCode
        <<",\"driver_handle\":"<<(job.driverHandle?"true":"false")<<",\"chronology\":[";
    for(size_t i=0;i<job.chronology.size();++i) std::cout<<(i?",":"")<<job.chronology[i];
    std::cout<<"],\"color_resources\":[";
    const auto colors=job.color_resources();
    for(size_t i=0;i<colors.size();++i) std::cout<<(i?",":"")<<"\""<<reinterpret_cast<uintptr_t>(colors[i])<<"\"";
    std::cout<<"],\"descriptor_heaps\":[";
    const auto heaps=job.descriptor_heaps();
    for(size_t i=0;i<heaps.size();++i) std::cout<<(i?",":"")<<"\""<<heaps[i]<<"\"";
    std::cout<<"],\"motion_resource\":\""<<(job.flow?reinterpret_cast<uintptr_t>(job.flow->motion):0)
        <<"\",\"depth_resource\":\""<<(job.flow?reinterpret_cast<uintptr_t>(job.flow->depth):0)
        <<"\",\"cleanup_pending\":true}"<<std::endl;
    if(job.runtimeModule) std::cout<<"NR_MODEL_MODULE {\"path\":\""<<std::filesystem::path(job.modulePath).generic_string()
        <<"\",\"base\":\""<<job.runtimeModule<<"\",\"selected_sha256\":\""<<h.config.runtime.sha256<<"\"}"<<std::endl;
    if((lane==0 || lane>=3) && step==0) require(job.created && job.submitted && job.runtimeModule,
        "native actual Feature18 creation failed; exact row preserved");
}
}
namespace {
class CoreBackend final : public nr::qa::CoreControlBackend {
    Harness h;
    void* state=nullptr;
    void(*dispatch)(void*)=nullptr;
    lifecycle::ModelClose cached;
    static void render(void* opaque,uint32_t x,uint32_t y) {
        auto& self=*static_cast<CoreBackend*>(opaque);
        if(self.h.action.load()!=Action::None)draw(&self.h,x,y);
        self.dispatch(self.state);
    }
public:
    explicit CoreBackend(const std::filesystem::path& root):h(root,model::Fault::AfterCoreInit,5) {}
    void register_callback(void* value,void(*fn)(void*)) override {
        state=value;dispatch=fn;obs_add_main_render_callback(render,this);
    }
    void remove_callback() override {
        obs_enter_graphics();obs_remove_main_render_callback(render,this);obs_leave_graphics();
    }
    bool initialize_capture() {
        h.execute(Action::Init);
        const auto renderer=interop::renderer_luid(h.renderer.Get());
        const auto worker=h.context->device()->GetAdapterLuid();
        const auto& adapter=h.adapterDescription;
        std::cout<<"NR_CORE_CONTROL_CONTEXT {\"instance\":"<<h.instance
            <<",\"renderer_luid_high\":"<<renderer.HighPart<<",\"renderer_luid_low\":"<<renderer.LowPart
            <<",\"context_luid_high\":"<<h.context->luid().HighPart<<",\"context_luid_low\":"<<h.context->luid().LowPart
            <<",\"worker_luid_high\":"<<worker.HighPart<<",\"worker_luid_low\":"<<worker.LowPart
            <<",\"device\":"<<reinterpret_cast<uintptr_t>(h.context->device())<<",\"queue\":"<<reinterpret_cast<uintptr_t>(h.context->queue())
            <<",\"vendor_id\":"<<adapter.VendorId<<",\"device_id\":"<<adapter.DeviceId
            <<",\"adapter_flags\":"<<adapter.Flags<<",\"software\":"<<((adapter.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)?"true":"false")
            <<",\"adapter_description_utf16\":[";
        for(size_t i=0;i<std::size(adapter.Description) && adapter.Description[i];++i){if(i)std::cout<<',';std::cout<<static_cast<uint16_t>(adapter.Description[i]);}
        std::cout<<"]}"<<std::endl;
        h.pixels=pattern(h.sequence+1);h.expectedHash=hash(h.pixels);
        h.execute(Action::Capture);
        require(h.samples.size()==1,"CORE control one capture");
        auto& sample=*h.samples.front();auto& job=*sample.job;
        require(sample.returned.wait_for(std::chrono::seconds(30))==std::future_status::ready,
            "CORE control exact dispatch return");
        require(WaitForSingleObject(job.observed,30000)==WAIT_OBJECT_0 && job.ready.load(),
            "CORE control expected-fault status event");
        require(job.fault==model::Fault::AfterCoreInit && job.code==0xe019 && job.diagnostic.empty() &&
            !job.created && !job.evaluated && !job.composed && !job.submitted && !job.eligible &&
            !job.resources && !job.runtimeModule && !h.processor->applied(),"CORE control no NR work");
        checked(h.context->queue()->Wait(job.frame->producer.fence,job.frame->producer.value),
            "CORE control actual producer dependency");
        const auto raw=readback(*h.context,job.frame->original.Get());
        const auto analysis=readback(*h.context,job.frame->analysis.Get());
        require(hash(raw)==sample.expected && hash(analysis)==sample.expected,"CORE control independent input binding");
        std::cout<<"NR_CORE_CONTROL_CAPTURE {\"captures\":1,\"fault_code\":"<<job.code
            <<",\"created\":false,\"evaluated\":false,\"composed\":false,\"submitted\":false,\"eligible\":false"
            <<",\"resources\":false,\"runtime_module\":false,\"applied\":false,\"dispatch_returned\":true,\"observed\":true"
            <<",\"raw_hash\":\""<<hash(raw)<<"\",\"analysis_hash\":\""<<hash(analysis)<<"\"}"<<std::endl;
        return true;
    }
    lifecycle::ModelClose diagnostics() const override {return h.processor?h.processor->close_diagnostics():cached;}
    nr::qa::CoreCloseOutcome close_step(nr::qa::CoreCloseStep step) override {
        using Outcome=nr::qa::CoreCloseOutcome;
        // An unresolved QA readback may still be reading this lane's source/context: refuse every step.
        if(!qa::core_close_admissible())return Outcome::Refused;
        switch(step) {
        case nr::qa::CoreCloseStep::Stop:
            if(!h.pipeline)return Outcome::Refused;
            if(!std::all_of(h.samples.begin(),h.samples.end(),[](const auto& s){return s->job->ready.load();}))return Outcome::Pending;
            if(!h.stopping)h.stop();
            if(!h.exited.valid())h.close();
            return h.exited.valid()?Outcome::Complete:Outcome::Refused;
        case nr::qa::CoreCloseStep::Worker:
            h.pipeline->service();cached=h.processor->close_diagnostics();
            if(h.exited.wait_for(std::chrono::seconds(0))!=std::future_status::ready || !cached.coherent)return Outcome::Pending;
            return h.processor->closed()?Outcome::Complete:Outcome::Refused;
        case nr::qa::CoreCloseStep::Generation:
            return h.pipeline->complete_generation(1)?Outcome::Complete:Outcome::Refused;
        case nr::qa::CoreCloseStep::Pipeline: {
            // Samples were released at Stop; refresh the same Bridge's exact tracked tickets.
            for(size_t slot=0;slot<3;++slot) {
                const auto physical=h.bridge->ownership(slot);
                if(!physical.key)continue;
                const auto before=h.pipeline->state(*physical.key);
                if(!before)continue;
                const interop::Ticket ticket{*physical.key,slot};
                auto diagnostic=[&](const char* phase,const pipeline::SlotState& state,const char* accepted) {
                    const auto& key=state.ticket.key;
                    std::cout<<"NR_PIPELINE_RECEIPT {\"refresh\":\""<<phase<<"\",\"observe_accepted\":"<<accepted
                        <<",\"instance_id\":"<<key.frame.instanceId<<",\"content_epoch\":"<<key.frame.contentEpoch
                        <<",\"ingress_sequence\":"<<key.frame.ingressSequence<<",\"history_epoch\":"<<key.historyEpoch
                        <<",\"resource_generation\":"<<key.resourceGeneration<<",\"submission_serial\":"<<key.submissionSerial
                        <<",\"slot\":"<<slot<<",\"tracked_slot\":"<<state.ticket.slot
                        <<",\"publication\":"<<static_cast<unsigned>(state.publication)
                        <<",\"dispatch_returned\":"<<(state.dispatchReturned?"true":"false");
                    auto ownership=[](const char* name,const interop::Ownership& value) {
                        std::cout<<",\""<<name<<"\":{\"phase\":"<<static_cast<unsigned>(value.phase)
                            <<",\"consumer_open\":"<<(value.consumerOpen?"true":"false")
                            <<",\"failed\":"<<(value.failed?"true":"false")<<",\"cancelled\":"<<(value.cancelled?"true":"false")
                            <<",\"producer_required\":"<<value.producer<<",\"producer_completed\":"<<value.producerDone
                            <<",\"worker_required\":"<<value.worker<<",\"worker_completed\":"<<value.workerDone
                            <<",\"consumer_required\":"<<value.consumer<<",\"consumer_completed\":"<<value.consumerDone<<'}';
                    };
                    ownership("cached",state.physical);ownership("bridge",physical);
                    std::cout<<",\"bridge_graphics_owner\":"<<(h.context->graphics_thread()==GetCurrentThreadId()?"true":"false")
                        <<",\"pipeline_graphics_owner\":\"UNKNOWN\",\"retired_generations\":"<<h.pipeline->retired_generations()
                        <<",\"worker_exit_ready\":"<<(h.exited.valid() && h.exited.wait_for(std::chrono::seconds(0))==std::future_status::ready?"true":"false")
                        <<",\"feature_complete\":\"UNKNOWN\"}"<<std::endl;
                };
                diagnostic("BEFORE",*before,"\"UNKNOWN\"");
                const bool accepted=h.pipeline->observe(ticket,physical);
                const auto after=h.pipeline->state(ticket.key);
                require(after.has_value(),"CORE control retained tracked receipt state");
                diagnostic("AFTER",*after,accepted?"true":"false");
                require(accepted,"CORE control exact Pipeline physical receipt");
            }
            h.pipeline->collect();if(!h.pipeline->try_close())return Outcome::Refused;h.pipeline.reset();return Outcome::Complete;
        }
        case nr::qa::CoreCloseStep::Bridge:
            if(!h.bridge->close())return Outcome::Refused;h.bridge.reset();return Outcome::Complete;
        case nr::qa::CoreCloseStep::Resources:
            cached=h.processor->close_diagnostics();h.processor.reset();
            h.samples.clear();h.admission.reset();h.target12.Reset();h.target11.Reset();
            if(h.source){gs_texture_destroy(h.source);h.source=nullptr;}
            h.context.reset();h.graphics.Reset();h.renderer.Reset();return Outcome::Complete;
        case nr::qa::CoreCloseStep::Count:break;
        }
        throw std::logic_error("CORE control close stage");
    }
};
}
bool nr_start_core_control(const std::filesystem::path& root,std::unique_ptr<nr::qa::CoreControlOwner>& owner) {
    auto backend=std::make_unique<CoreBackend>(root);auto* borrowed=backend.get();
    owner=std::make_unique<nr::qa::CoreControlOwner>(std::move(backend));
    owner->attach();
    return borrowed->initialize_capture();
}
bool nr_run_model_scenario(const std::filesystem::path& root,bool& retained) {
    // This function runs on the coordinator, not the graphics/NR worker.
    nr_color_refuse_unrelated();
    const model::Fault controls[]{model::Fault::None,model::Fault::Capability,model::Fault::Creation,
        model::Fault::Evaluation,model::Fault::Submission};
    for(unsigned lane=0;lane<5;++lane) {
        auto h=std::make_unique<Harness>(root,controls[lane],lane);
        bool callback=false;
        try {
            nr_color_refuse_unrelated();
            obs_add_main_render_callback(draw,h.get()); callback=true; h->execute(Action::Init);
            std::cout<<"NR_MODEL_CONTEXT {\"lane\":"<<lane<<",\"adapter\":\"RTX 4090\",\"software\":false,\"private_direct_queue\":true"
                <<",\"renderer_luid_high\":"<<h->context->luid().HighPart<<",\"renderer_luid_low\":"<<h->context->luid().LowPart
                <<",\"worker_luid_high\":"<<h->context->device()->GetAdapterLuid().HighPart
                <<",\"worker_luid_low\":"<<h->context->device()->GetAdapterLuid().LowPart
                <<",\"device\":\""<<reinterpret_cast<uintptr_t>(h->context->device())<<"\",\"queue\":\""
                <<reinterpret_cast<uintptr_t>(h->context->queue())<<"\"}"<<std::endl;
            const unsigned count=lane==0?6:lane<3?1:3;
            for(unsigned step=0;step<count;++step) {
                // Creation is a warm-up, not an inference-history sample. Retire
                // that temporal epoch before cold/reset inference, including any
                // real canonical ticks consumed by driver/model initialization.
                if(((lane==0 || lane>=3) && step==1) || (lane==0 && step==4)) h->execute(Action::Cut);
                h->pixels=pattern(h->sequence+1); h->expectedHash=hash(h->pixels);
                h->execute(Action::Capture); proof(*h,lane,step);
                if(h->samples.size()>2) h->samples.erase(h->samples.begin());
            }
            const auto applied=h->processor->applied();
            if(lane==0) require(h->eligibleJobs==5 && applied==3,"native five physical inferences and three actual n-1 publications");
            else require(applied==0,"native failed capability/create/evaluation/submit cannot apply compose");
            h->execute(Action::Stop); if(h->consumerValue) require(h->consumerEnd &&
                WaitForSingleObject(h->consumerComplete->handle(),30000)==WAIT_OBJECT_0 &&
                interop::completion(h->consumerEnd->completionValue,h->consumerEnd->consumer.fence->GetCompletedValue())==
                    interop::Completion::Complete,"native stop exact consumer completion");
            h->execute(Action::Close);
            require(h->exited.wait_for(std::chrono::seconds(30))==std::future_status::ready,"native subscribed worker exit");
            h->execute(Action::Close); obs_remove_main_render_callback(draw,h.get()); callback=false;
            std::cout<<"NR_MODEL_CLEANUP {\"lane\":"<<lane<<",\"applied\":"<<applied<<",\"worker_exit\":true"
                <<",\"closed\":true,\"retained\":false,\"nr_selections\":"<<h->nrSelections<<",\"raw_selections\":"<<h->rawSelections<<"}"<<std::endl;
        } catch(const std::exception& error) {
            std::cout<<"NR_MODEL_FAILURE "<<error.what()<<std::endl;
            // A lane that published no sample proves nothing about the model graph, and a
            // retained host owner always blocks the settled path: only an exact published
            // sample set with no unresolved ownership may run the normal cleanup.
            const bool settled=qa::model_cleanup_settled(!h->samples.empty(),h->pipeline &&
                std::all_of(h->samples.begin(),h->samples.end(),[](const auto& sample) {
                    return sample->job->ready.load(std::memory_order_acquire);
                }));
            if(settled) {
                try {
                    h->failed=false; h->error.clear();
                    h->execute(Action::Stop); if(h->consumerValue) require(h->consumerEnd &&
                        WaitForSingleObject(h->consumerComplete->handle(),30000)==WAIT_OBJECT_0 &&
                        interop::completion(h->consumerEnd->completionValue,h->consumerEnd->consumer.fence->GetCompletedValue())==
                            interop::Completion::Complete,"failed native stop exact consumer completion");
                    h->execute(Action::Close);
                    require(h->exited.wait_for(std::chrono::seconds(30))==std::future_status::ready,"failed native exact worker exit");
                    h->execute(Action::Close);
                    if(callback) obs_remove_main_render_callback(draw,h.get());
                    std::cout<<"NR_MODEL_FAILED_CLEANUP exact=true retained=false"<<std::endl;
                    return false;
                } catch(const std::exception& cleanup) {
                    std::cout<<"NR_MODEL_CLEANUP_FAILURE "<<cleanup.what()<<std::endl;
                }
            }
            // A failed action is not normal cleanup. Keep exact code/context/resources
            // and callback owner; runner retains this host instead of forced unloading.
            if(callback) obs_remove_main_render_callback(draw,h.get());
            std::cout<<"NR_MODEL_RETAINED unresolved=true pid="<<GetCurrentProcessId()<<std::endl;
            // The exact lane owner moves into process-lifetime custody, including when no
            // sample published an observed HANDLE. A null retained HANDLE is never
            // permission to unload libobs, so the coordinator is told unresolved too.
            auto* owner=h.get();
            qa::retain_unresolved_model(h);
            retained=true;
            // Subscribe to the retained job's exact already-created event, never
            // an invented readiness signal. No forced unload on unknown completion.
            const HANDLE observed=owner->samples.empty()?nullptr:owner->samples.back()->job->observed;
            if(observed && WaitForSingleObject(observed,INFINITE)==WAIT_OBJECT_0) {
                ResetEvent(owner->acted.value);
                WaitForSingleObject(owner->acted.value,INFINITE); // Explicit retained-owner checkpoint, not GPU readiness.
            }
            return false;
        }
    }
    std::cout<<"NR_MODEL_SCOPE Feature18=true integrated=true quality=false performance=false recording=false cleanup=complete"<<std::endl;
    return true;
}
