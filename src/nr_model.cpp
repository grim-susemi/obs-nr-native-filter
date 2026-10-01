// SPDX-License-Identifier: GPL-3.0-only
// Driver ABI and Feature18 use R2 / tests/nr_compatibility/HardwareSmoke.cpp.
#include "nr_model.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <thread>
using Microsoft::WRL::ComPtr;
namespace nr::model {
namespace {
void checked(HRESULT hr,const char* what) { if(FAILED(hr)) throw std::runtime_error(what); }
void require(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
int64_t stamp() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
template<class T> T symbol(HMODULE module,const char* name) {
    auto value=GetProcAddress(module,name); require(value!=nullptr,name);
    return reinterpret_cast<T>(value);
}
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    list->ResourceBarrier(1,&b);
}
ComPtr<ID3D12Resource> texture(ID3D12Device* device,Extent size,DXGI_FORMAT format) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=size.width; desc.Height=size.height; desc.DepthOrArraySize=desc.MipLevels=1;
    desc.Format=format; desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&result)),"model color allocation");
    return result;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* device,D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=4; desc.Height=1; desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(type==D3D12_HEAP_TYPE_DEFAULT) desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&result)),"model status allocation");
    return result;
}
ComPtr<ID3D12DescriptorHeap> heap(ID3D12Device* device,UINT count) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors=count; desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> result;
    checked(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&result)),"model private descriptor heap");
    return result;
}
struct Driver;
thread_local Driver* currentDriver=nullptr; // Exact private worker call scope, no provider registry.
struct Driver {
    ComPtr<ID3D12Device> device;
    HMODULE module=nullptr;
    runtime::Dispatch dispatch;
    decltype(dispatch.create) createRaw=nullptr;
    uint32_t createCode=0;
    bool createHandle=false;
    // _nvngx core export, not the public SDK/model one-argument wrapper.
    using Shutdown=NVSDK_NGX_Result (*)(ID3D12Device*,unsigned int&);
    Shutdown shutdown=nullptr;
    bool initialized=false;
    ~Driver() {
        if(initialized) {
            OutputDebugStringA("NR driver initialization retained without successful worker shutdown");
            (void)device.Detach(); module=nullptr;
        }
        if(module) FreeLibrary(module);
    }
    bool close() {
        unsigned remaining=0;
        if(initialized && (shutdown(device.Get(),remaining)!=NVSDK_NGX_Result_Success || remaining!=0)) return false;
        initialized=false;
        if(module && !FreeLibrary(module)) return false;
        module=nullptr; return true;
    }
    static NVSDK_NGX_Result create(ID3D12GraphicsCommandList* commands,NVSDK_NGX_Feature feature,
        NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** handle) {
        require(currentDriver!=nullptr,"private driver create call owner");
        const auto code=currentDriver->createRaw(commands,feature,params,handle);
        currentDriver->createCode=static_cast<uint32_t>(code);
        currentDriver->createHandle=handle && *handle;
        return code;
    }
    static std::shared_ptr<Driver> open(ID3D12Device* device,const Config& config) {
        auto result=std::make_shared<Driver>(); result->device=device;
        require(config.driver.is_absolute() && std::filesystem::canonical(config.driver)==config.driver.lexically_normal(),
            "canonical owned driver path required");
        result->module=LoadLibraryExW(config.driver.c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(result->module!=nullptr,"private NGX driver load");
        // Core dispatcher Init_Ext takes FeatureCommonInfo, unlike direct model Init_Ext.
        using Init=NVSDK_NGX_Result (*)(unsigned long long,const wchar_t*,ID3D12Device*,
            NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
        auto init=symbol<Init>(result->module,"NVSDK_NGX_D3D12_Init_Ext");
        auto& d=result->dispatch;
        d.allocate=symbol<decltype(d.allocate)>(result->module,"NVSDK_NGX_D3D12_GetCapabilityParameters");
        d.destroy=symbol<decltype(d.destroy)>(result->module,"NVSDK_NGX_D3D12_DestroyParameters");
        result->createRaw=symbol<decltype(d.create)>(result->module,"NVSDK_NGX_D3D12_CreateFeature");
        d.create=&Driver::create;
        d.evaluate=symbol<decltype(d.evaluate)>(result->module,"NVSDK_NGX_D3D12_EvaluateFeature");
        d.release=symbol<decltype(d.release)>(result->module,"NVSDK_NGX_D3D12_ReleaseFeature");
        result->shutdown=symbol<decltype(result->shutdown)>(result->module,"NVSDK_NGX_D3D12_Shutdown1");
        // CORE capability discovery must not scan the installed OBS directory
        // and load its unrelated SR DLL. The selected NR image is opened by the
        // accepted direct fallback only after actual driver create failure.
        const auto directory=config.data.wstring();
        const wchar_t* paths[]{directory.c_str()};
        NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path=paths; info.PathListInfo.Length=1;
        const auto code=init(0x24480451ull,config.data.c_str(),device,NVSDK_NGX_Version_API,&info);
        if(code!=NVSDK_NGX_Result_Success) throw std::runtime_error("private NGX driver Init_Ext result="+std::to_string(static_cast<uint32_t>(code)));
        result->initialized=true; return result;
    }
};
}
Frame::Frame(ID3D12Resource* raw,ID3D12Resource* flow,std::shared_ptr<void> lease,NV_OF_FENCE_POINT ready)
    :original(raw),analysis(flow),producerLease(ready.fence),graphicsLease(std::move(lease)),producer(ready) {
    require(raw && flow && raw!=flow && graphicsLease && ready.fence && ready.value,"immutable model frame required");
}
Job::Job(std::shared_ptr<const Frame> input,const JobContract& sample,ColorExposure value,Fault control)
    :frame(std::move(input)),contract(sample),exposure(value),fault(control),
      observed(CreateEventW(nullptr,TRUE,FALSE,nullptr)),gpu(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    require(frame && observed && gpu,"model job and exact completion events");
}
Job::~Job() { if(observed) CloseHandle(observed); if(gpu) CloseHandle(gpu); }
struct Job::Resources {
    const std::shared_ptr<interop::Bridge> bridge;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> creation;
    std::array<ComPtr<ID3D12Resource>,5> colors;
    ComPtr<ID3D12Resource> status, statusRead, zero;
    ComPtr<ID3D12DescriptorHeap> codecHeap,prepareHeap,restoreHeap;
    std::unique_ptr<D3D12ColorPass> color;
    std::unique_ptr<Core> core;
    ComPtr<ID3D12Fence> complete;
    std::shared_ptr<runtime::Submission> submission=std::make_shared<runtime::Submission>();
    HANDLE wait=nullptr;
    bool statusRecorded=false;
    explicit Resources(std::shared_ptr<interop::Bridge> transport):bridge(std::move(transport)) {}
    void unsubscribe() {
        if(wait) { require(UnregisterWaitEx(wait,INVALID_HANDLE_VALUE)!=FALSE,"model completion unsubscribe"); wait=nullptr; }
    }
};
NV_OF_FENCE_POINT Job::completion() const { return resources?NV_OF_FENCE_POINT{resources->complete.Get(),1}:NV_OF_FENCE_POINT{}; }
ID3D12Resource* Job::invalid_status_resource() const { return resources?resources->status.Get():nullptr; }
std::array<ID3D12Resource*,5> Job::color_resources() const {
    std::array<ID3D12Resource*,5> result{};
    if(resources) for(size_t i=0;i<result.size();++i) result[i]=resources->colors[i].Get();
    return result;
}
std::array<uintptr_t,3> Job::descriptor_heaps() const {
    return resources?std::array<uintptr_t,3>{reinterpret_cast<uintptr_t>(resources->codecHeap.Get()),
        reinterpret_cast<uintptr_t>(resources->prepareHeap.Get()),reinterpret_cast<uintptr_t>(resources->restoreHeap.Get())}:
        std::array<uintptr_t,3>{};
}
namespace {
void CALLBACK completed(void* opaque,BOOLEAN) {
    std::unique_ptr<std::shared_ptr<Job>> retained(static_cast<std::shared_ptr<Job>*>(opaque));
    auto& job=**retained; auto& r=*job.resources;
    if(r.complete->GetCompletedValue()==UINT64_MAX) return; // Unknown ownership never becomes ready.
    job.chronology[6]=stamp();
    if(r.statusRecorded) {
        void* bytes=nullptr; const D3D12_RANGE range{0,4};
        if(FAILED(r.statusRead->Map(0,&range,&bytes))) { job.code=0xe001; job.invalid=UINT32_MAX; }
        else { memcpy(&job.invalid,bytes,4); const D3D12_RANGE empty{0,0}; r.statusRead->Unmap(0,&empty); }
    }
    job.eligible=job.evaluated && job.composed && job.submitted && !job.invalid &&
        r.submission->state()==runtime::Completion::Complete;
    job.chronology[7]=stamp();
    job.ready.store(true,std::memory_order_release);
    SetEvent(job.observed);
}
runtime::Recording recording(const std::shared_ptr<Job>& job,ID3D12GraphicsCommandList* commands) {
    auto submission=job->resources->submission;
    return {commands,job->epoch,job,[submission]{return submission->state();}};
}
struct Evaluator final : ModelEvaluator {
    runtime::Pass& pass;
    const std::weak_ptr<Job> current;
    Evaluator(runtime::Pass& value,const std::shared_ptr<Job>& job):pass(value),current(job) {}
    EvaluationResult evaluate(const ModelInput& in) override {
        auto job=current.lock(); require(job!=nullptr,"live model evaluation lease");
        if(job->fault==Fault::Evaluation) return {EvaluationState::Error,0xe018};
        const runtime::Inputs inputs{static_cast<ID3D12Resource*>(in.color.native),
            static_cast<ID3D12Resource*>(in.depth.native),static_cast<ID3D12Resource*>(in.motion.native),
            static_cast<ID3D12Resource*>(in.output.native),in.depth.active,in.motion.active,false,
            in.input.history.reset,in.input.mvScaleX,in.input.mvScaleY,true};
        const auto result=pass.run(in.input.job,inputs,recording(job,
            static_cast<ID3D12GraphicsCommandList*>(in.input.commands)));
        job->chronology[2]=stamp();
        return {result==NVSDK_NGX_Result_Success?EvaluationState::Evaluated:EvaluationState::Error,
            static_cast<uint32_t>(result)};
    }
};
struct TracedCodec final : Codec {
    D3D12Codec codec;
    Job& job;
    TracedCodec(ID3D12Device* device,std::span<const std::byte> shader,Job& value):codec(device,shader),job(value) {}
    bool encode(const CoreInput& input,const CodecConstants& constants) override {
        const bool result=codec.encode(input,constants); job.chronology[1]=stamp(); return result;
    }
    bool resolve(const CoreInput& input,const CodecConstants& constants) override {
        const bool result=codec.resolve(input,constants); job.chronology[3]=stamp(); return result;
    }
};
bool typed(ID3D12Device* device,DXGI_FORMAT format,bool store) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT caps{format};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&caps,sizeof(caps))) &&
        (caps.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD) &&
        (!store || (caps.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE));
}
}
struct Processor::Impl {
    const std::shared_ptr<interop::Context> context;
    const Config config;
    const uint64_t instance;
    std::thread::id worker;
    std::shared_ptr<Driver> driver;
    std::shared_ptr<runtime::Module> module;
    std::unique_ptr<runtime::Pass> pass;
    std::unique_ptr<D3D12Guides> guides;
    std::optional<Settings> settings;
    std::optional<Extent> extent;
    std::optional<JobKey> previousKey;
    std::shared_ptr<const Frame> previousFrame;
    std::shared_ptr<Job> pending;
    std::optional<JobKey> pendingGuide;
    std::atomic<uint64_t> appliedCount{0};
    std::atomic<bool> closed{false};
    uint64_t epoch=0;
    uint64_t creationEpoch=0;
    uint64_t generation=0;
    bool reset=true;
    Impl(std::shared_ptr<interop::Context> value,Config input,uint64_t id)
        :context(std::move(value)),config(std::move(input)),instance(id) {
        require(context && id && !config.codec.empty() && !config.color.empty() && !config.guides.empty(),
            "private context and actual compiled model shaders required");
    }
    bool settle() {
        if(!pending) return true;
        if(!pending->ready.load(std::memory_order_acquire)) return false;
        if(pendingGuide && !guides->collect(*pendingGuide,{pending->resources->complete.Get(),1})) return false;
        pendingGuide.reset(); pending->resources->unsubscribe(); pending.reset(); return true;
    }
    void subscribe(const std::shared_ptr<Job>& job) {
        checked(context->device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&job->resources->complete)),
            "model completion fence");
        checked(job->resources->complete->SetEventOnCompletion(1,job->gpu),"model pre-trigger fence subscription");
        auto retained=std::make_unique<std::shared_ptr<Job>>(job);
        require(RegisterWaitForSingleObject(&job->resources->wait,job->gpu,completed,retained.get(),
            INFINITE,WT_EXECUTEONLYONCE)!=FALSE,"model pre-trigger completion observer");
        retained.release();
    }
    void signal(const std::shared_ptr<Job>& job) {
        checked(context->queue()->Signal(job->resources->complete.Get(),1),"model retained completion signal");
        pending=job;
    }
};
Processor::Processor(std::shared_ptr<interop::Context> context,Config config,uint64_t instance)
    :impl_(std::make_unique<Impl>(std::move(context),std::move(config),instance)) {}
Processor::~Processor() {
    if(!impl_->closed.load(std::memory_order_acquire)) {
        OutputDebugStringA("NR model processor retained: worker cleanup not complete");
        (void)impl_.release();
    }
}
bool Processor::closed() const { return impl_->closed.load(std::memory_order_acquire); }
uint64_t Processor::applied() const { return impl_->appliedCount.load(std::memory_order_acquire); }
bool Processor::publish(const std::shared_ptr<Job>& job,const Selection& selected,const interop::Ownership& physical) {
    if(GetCurrentThreadId()!=impl_->context->graphics_thread() || !job || !job->ready.load(std::memory_order_acquire) ||
        !job->eligible || selected.kind!=OutputKind::DelayedNr || !selected.job || *selected.job!=job->contract.key ||
        !selected.frame || *selected.frame!=job->contract.key.frame || selected.canonicalTick!=job->contract.deadlineTick ||
        !interop::publishable(physical,job->contract.key) || physical.consumerOpen) return false;
    bool expected=false;
    if(!job->published.compare_exchange_strong(expected,true)) return false;
    impl_->appliedCount.fetch_add(1,std::memory_order_relaxed); return true;
}
pipeline::Processed Processor::process(const pipeline::Work& work,std::stop_token stop) {
    auto& p=*impl_;
    if(p.worker==std::thread::id{}) p.worker=std::this_thread::get_id();
    require(p.worker==std::this_thread::get_id(),"single scheduler worker owns model");
    auto job=std::static_pointer_cast<Job>(work.binding.processorLease);
    require(job && job->contract==work.job && work.raw && work.job.input==work.raw->input && work.binding.transport &&
        work.binding.ticket.key==work.job.key && work.binding.capturePin &&
        work.binding.capturePin->ticket==work.binding.ticket,"exact captured model binding");
    auto fail=[&](uint32_t code) {
        job->code=code; p.reset=true;
        require(work.binding.transport->discard_recording(work.binding.ticket),"model presubmit discard");
        job->ready.store(true,std::memory_order_release); SetEvent(job->observed);
        return pipeline::Processed{work.job,false,{}};
    };
    try {
    if(stop.stop_requested() || !p.settle()) return fail(0xe002);
    job->epoch=++p.epoch; job->creationEpoch=p.creationEpoch;
    if(job->fault==Fault::Capability) return fail(0xe003); // Owned admission control, no feature/compose.
    const auto producer=job->frame->producer;
    const auto done=producer.fence->GetCompletedValue();
    if(done==UINT64_MAX) return fail(0xe004);
    if(done<producer.value) {
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"producer event");
        const auto result=producer.fence->SetEventOnCompletion(producer.value,event);
        const auto wait=SUCCEEDED(result)?WaitForSingleObject(event,15000):WAIT_FAILED;
        CloseHandle(event);
        if(wait!=WAIT_OBJECT_0 || producer.fence->GetCompletedValue()==UINT64_MAX) return fail(0xe005);
    }
    const auto& d=work.job.input.descriptor;
    const Extent size{d.active.width,d.active.height};
    auto* device=p.context->device();
    const auto plan=color_plan(d,job->exposure,
        typed(device,interop::source_format(d.format),false) &&
        typed(device,DXGI_FORMAT_R16G16B16A16_FLOAT,true) &&
        typed(device,interop::canonical_format(d.format),true));
    if(!plan) return fail(0xe006);
    const bool adapterChanged=p.settings!=work.job.input.settings || p.extent!=size ||
        p.generation!=work.job.key.resourceGeneration;
    if(p.pass && (adapterChanged || !p.pass->ready(job->epoch))) {
        if(!p.pass->release()) return fail(0xe007);
        p.pass.reset(); p.previousFrame.reset(); p.previousKey.reset();
        if(adapterChanged && p.guides) {
            require(p.guides->close(),"old generation guides close"); p.guides.reset();
        }
        p.reset=true;
    }
    if(!p.module) {
        p.driver=Driver::open(device,p.config);
        currentDriver=p.driver.get();
        p.module=runtime::Module::open(p.config.runtime,p.driver->dispatch,p.driver,device,p.config.data,
            runtime::Route::DriverFallback);
        require(p.module!=nullptr,"selected runtime module admission");
    }
    job->resources=std::make_shared<Job::Resources>(work.binding.transport);
    auto& r=*job->resources;
    if(!p.pass) {
        p.pass=std::make_unique<runtime::Pass>(p.module);
        p.settings.emplace(work.job.input.settings); p.extent.emplace(size);
        p.generation=work.job.key.resourceGeneration;
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r.allocator)),
            "owned creation allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r.allocator.Get(),nullptr,
            IID_PPV_ARGS(&r.creation)),"owned creation list");
        p.subscribe(job);
        if(job->fault==Fault::Creation) {
            r.submission->discard(); checked(r.creation->Close(),"failed creation close");
            work.binding.transport->discard_recording(work.binding.ticket);
            job->code=0xe008; p.signal(job); return {work.job,false,{}};
        }
        const auto code=p.pass->prepare(work.job.input.settings,size,work.job.key.historyEpoch,recording(job,r.creation.Get()));
        job->driverCreateCode=p.driver->createCode; job->driverHandle=p.driver->createHandle;
        HMODULE image=nullptr;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,p.config.runtime.canonicalPath.c_str(),&image)) {
            job->runtimeModule=reinterpret_cast<uintptr_t>(image);
            std::array<wchar_t,32768> path{};
            const auto count=GetModuleFileNameW(image,path.data(),static_cast<DWORD>(path.size()));
            if(count && count<path.size()) job->modulePath.assign(path.data(),count);
        }
        job->chronology[0]=stamp();
        p.creationEpoch=job->creationEpoch=job->epoch;
        job->code=static_cast<uint32_t>(code); job->created=code==NVSDK_NGX_Result_Success;
        checked(r.creation->Close(),"owned creation close");
        ID3D12CommandList* lists[]{r.creation.Get()}; p.context->queue()->ExecuteCommandLists(1,lists);
        job->chronology[4]=stamp();
        job->submitted=r.submission->submitted(p.context->queue());
        job->chronology[5]=stamp();
        require(work.binding.transport->discard_recording(work.binding.ticket),"creation raw slot discard");
        p.signal(job);
        return {work.job,false,{}};
    }
    if(!p.pass->ready(job->epoch)) return fail(0xe009);
    job->driverCreateCode=p.driver->createCode; job->driverHandle=p.driver->createHandle;
    HMODULE image=nullptr;
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,p.config.runtime.canonicalPath.c_str(),&image),
        "live selected Feature18 module");
    job->runtimeModule=reinterpret_cast<uintptr_t>(image);
    std::array<wchar_t,32768> path{};
    const auto count=GetModuleFileNameW(image,path.data(),static_cast<DWORD>(path.size()));
    require(count && count<path.size(),"live selected module path"); job->modulePath.assign(path.data(),count);
    const bool reset=p.reset || !p.previousKey || p.previousKey->historyEpoch!=work.job.key.historyEpoch ||
        p.previousKey->frame.contentEpoch!=work.job.key.frame.contentEpoch ||
        p.previousKey->frame.ingressSequence+1!=work.job.key.frame.ingressSequence;
    if(!p.guides) p.guides=std::make_unique<D3D12Guides>(device,p.context->queue(),d.allocation,p.config.guides);
    const FlowImage current{work.job.key,job->frame->analysis.Get(),producer};
    std::optional<FlowImage> previous;
    if(!reset) previous.emplace(FlowImage{*p.previousKey,p.previousFrame->analysis.Get(),p.previousFrame->producer});
    const auto flow=p.guides->submit({current,previous?&*previous:nullptr,d.allocation,d.active,reset});
    if(flow!=FlowProblem::None) return fail(0xe100+static_cast<uint32_t>(flow));
    const auto* guide=p.guides->result(); require(guide && guide->key==work.job.key,"exact native guides");
    job->flow.emplace(*guide);
    p.pendingGuide.emplace(work.job.key);
    checked(p.context->queue()->Wait(guide->complete.fence,guide->complete.value),"native guide queue dependency");
    const auto view=work.binding.transport->begin_worker(work.binding.ticket);
    require(view.has_value(),"actual Bridge worker admission");
    job->output=view->output;
    for(auto& color:r.colors) color=texture(device,size,DXGI_FORMAT_R16G16B16A16_FLOAT);
    r.status=buffer(device,D3D12_HEAP_TYPE_DEFAULT); r.statusRead=buffer(device,D3D12_HEAP_TYPE_READBACK);
    r.zero=buffer(device,D3D12_HEAP_TYPE_UPLOAD);
    r.codecHeap=heap(device,10); r.prepareHeap=heap(device,4); r.restoreHeap=heap(device,4);
    auto* list=view->commands;
    void* mapped=nullptr; const D3D12_RANGE empty{0,0};
    checked(r.zero->Map(0,&empty,&mapped),"owned status zero upload");
    memset(mapped,0,4); r.zero->Unmap(0,nullptr);
    list->CopyBufferRegion(r.status.Get(),0,r.zero.Get(),0,4);
    barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(list,job->frame->original.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    r.color=std::make_unique<D3D12ColorPass>(device,p.config.color);
    barrier(list,r.colors[2].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    require(r.color->record(list,r.prepareHeap.Get(),*plan,job->frame->original.Get(),
        r.colors[2].Get(),r.colors[0].Get(),r.status.Get(),false),"actual canonical color prepare");
    barrier(list,r.colors[2].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(list,r.colors[0].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto ct=[&](ID3D12Resource* value){return CoreTexture{value,size,{0,0,size.width,size.height}};};
    const CoreResources resources{ct(r.colors[0].Get()),ct(r.colors[1].Get()),ct(r.colors[2].Get()),
        ct(r.colors[3].Get()),ct(r.colors[4].Get()),
        {guide->depth,d.allocation,d.active},{guide->motion,d.allocation,d.active},r.codecHeap.Get()};
    // Foreign evaluator consumes explicit readable guides, leaving model output UAV.
    barrier(list,guide->depth,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(list,guide->motion,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const Epochs epochs{work.job.key.frame.instanceId,work.job.key.frame.contentEpoch,
        work.job.key.historyEpoch,work.job.key.resourceGeneration};
    const CoreInput input{work.job,epochs,resources,{work.job.key.historyEpoch,!reset,reset},list,
        plan->whitePoint,1,1,plan->codecColor,plan->codecAlpha};
    r.core=std::make_unique<Core>(p.instance,std::make_unique<TracedCodec>(device,p.config.codec,*job),
        std::make_unique<Evaluator>(*p.pass,job));
    p.subscribe(job);
    const auto result=r.core->record(input);
    job->evaluated=result.evaluated; job->composed=result.composed; job->code=result.error;
    if(result.state!=CoreState::Recorded || job->fault==Fault::Submission) {
        require(r.submission->discard(),"unsubmitted evaluation discard");
        require(work.binding.transport->discard_recording(work.binding.ticket),"failed core Bridge discard");
        if(job->fault==Fault::Submission) job->code=0xe010;
        p.reset=true; p.signal(job); return {work.job,false,{}};
    }
    barrier(list,r.colors[4].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(list,view->output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER order{}; order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; order.UAV.pResource=r.status.Get();
    list->ResourceBarrier(1,&order);
    require(r.color->record(list,r.restoreHeap.Get(),*plan,job->frame->original.Get(),r.colors[4].Get(),
        view->output,r.status.Get(),true),"actual source-relative color restore");
    barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(r.statusRead.Get(),0,r.status.Get(),0,4); r.statusRecorded=true;
    barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,job->frame->original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,guide->depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,guide->motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,view->output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    for(size_t i=0;i<r.colors.size();++i) barrier(list,r.colors[i].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    job->submitted=work.binding.transport->submit_worker(work.binding.ticket);
    job->chronology[4]=stamp();
    if(job->submitted) job->submitted=r.submission->submitted(p.context->queue());
    job->chronology[5]=stamp();
    p.reset=!job->submitted;
    p.previousKey.emplace(work.job.key); p.previousFrame=job->frame;
    p.signal(job);
    return {work.job,job->submitted,job};
    } catch(const std::exception& error) {
        job->diagnostic=error.what(); job->code=0xefff; p.reset=true;
        // No recording/resources means no feature submission occurred. All other
        // failures retain their exact resource owners; never manufacture completion.
        if(!job->resources && work.binding.transport->discard_recording(work.binding.ticket)) {
            job->ready.store(true,std::memory_order_release); SetEvent(job->observed);
        }
        return {work.job,false,{}};
    }
}
void Processor::worker_stopping() noexcept {
    auto& p=*impl_;
    try {
        if(p.worker==std::thread::id{}) p.worker=std::this_thread::get_id();
        require(p.worker==std::this_thread::get_id(),"model cleanup on its worker");
        if(!p.settle()) return;
        if(p.pass && !p.pass->release()) return;
        p.pass.reset(); p.previousFrame.reset(); p.previousKey.reset();
        if(p.guides && !p.guides->close()) return;
        p.guides.reset(); p.module.reset();
        HMODULE retained=nullptr;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,p.config.runtime.canonicalPath.c_str(),&retained))
            return; // Actual owned direct image still loaded: do not assert successful module retirement.
        if(p.driver && !p.driver->close()) return;
        p.driver.reset(); p.closed.store(true,std::memory_order_release);
        currentDriver=nullptr;
    } catch(...) { OutputDebugStringA("NR model worker shutdown retained exact owner"); }
}
} // namespace nr::model
