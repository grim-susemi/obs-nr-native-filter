// SPDX-License-Identifier: GPL-3.0-only
// Driver ABI and Feature18 use R2 / tests/nr_compatibility/HardwareSmoke.cpp.
#include "nr_model.h"
#include <string>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <array>
#include <mutex>
#include <stdexcept>
#include <thread>
extern "C" IMAGE_DOS_HEADER __ImageBase;
using Microsoft::WRL::ComPtr;
namespace nr::model {
// Heuristic: export served by another image, or starting with a detour jump.
bool entry_interposed(HMODULE owner,const void* entry) {
    if(!owner||!entry)return false;
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(entry,&info,sizeof info))return false;
    if(info.AllocationBase!=reinterpret_cast<void*>(owner))return true;
    const auto* code=static_cast<const unsigned char*>(entry);
    return code[0]==0xE9||(code[0]==0xFF&&code[1]==0x25);
}
namespace {
std::atomic<uint64_t> signalLive{0};
class ModuleSignal final:public QaSignal {
    std::shared_future<void> future_;
public:
    ModuleSignal(){signalLive.fetch_add(1,std::memory_order_relaxed);}
    ~ModuleSignal() override {
        future_={}; // Final state-manager release runs in the allocating module.
        signalLive.fetch_sub(1,std::memory_order_release);
    }
    void bind(const std::shared_future<void>& value) noexcept override {future_=value;}
    bool ready() const noexcept override {
        return future_.valid()&&future_.wait_for(std::chrono::seconds(0))==std::future_status::ready;
    }
    bool wait_for(uint32_t milliseconds) const override {
        return future_.valid()&&future_.wait_for(std::chrono::milliseconds(milliseconds))==std::future_status::ready;
    }
    uintptr_t owner_module() const noexcept override {return reinterpret_cast<uintptr_t>(&__ImageBase);}
};
void checked(HRESULT hr,const char* what) { if(FAILED(hr)) throw interop::HResultError(hr,what); }
void require(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
int64_t stamp() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
struct TimingAgg {
    std::atomic<unsigned> jobs{0};
    std::atomic<int64_t> sumSubmit{0},maxSubmit{0},sumEncode{0},maxEncode{0},sumAdmit{0},maxAdmit{0};
    std::atomic<unsigned> admitJobs{0},late{0},stageJobs{0},queueJobs{0},creationJobs{0};
    std::atomic<int64_t> sumCreation{0},maxCreation{0};
    std::atomic<int64_t> sumProd{0},maxProd{0},sumPrep{0},sumRec{0},sumGpu{0},sumQueue{0},maxQueue{0};
};
TimingAgg timingAgg;
struct JobTraceEntry {std::atomic<uint64_t> deadline{0};std::atomic<int64_t> queue{0},run{0},total{0};std::atomic<int> late{0};};
JobTraceEntry jobTrace[16];
std::atomic<unsigned> jobTraceHead{0};
struct EligEntry {std::atomic<uint64_t> deadline{0};std::atomic<uint32_t> invalid{0},code{0},flags{0};}; // diagnostic: publication eligibility
EligEntry eligTrace[16];
std::atomic<unsigned> eligHead{0};
struct EligAgg {std::atomic<unsigned> n{0},bad{0},badHist{0},badReset{0},orMask{0},noEval{0},noSubmit{0},incomplete{0},qa{0};};
EligAgg eligAgg;
std::atomic<uint32_t> appliedPerf{0},appliedGrid{0},appliedHint{0}; // diagnostic: estimator settings actually in use
struct TickRing {std::atomic<uint64_t> tick[256];std::atomic<int64_t> qpc[256];};
TickRing tickRing;
int64_t admit_stamp(uint64_t tick) {
    if(!tick) return 0;
    const auto i=tick&255;
    if(tickRing.tick[i].load(std::memory_order_acquire)!=tick) return 0;
    return tickRing.qpc[i].load(std::memory_order_relaxed);
}
void raise_max(std::atomic<int64_t>& slot,int64_t value) {
    auto seen=slot.load(std::memory_order_relaxed);
    while(value>seen && !slot.compare_exchange_weak(seen,value,std::memory_order_relaxed)) {}
}
void record_timing(const std::array<int64_t,8>& c,uint64_t deadline,const std::array<int64_t,5>& q) { // diagnostic only: evaluation jobs carry an encode stamp
    if(c[1]<=0 && c[0]>0 && c[5]>0 && c[6]>=c[0]) {
        timingAgg.creationJobs.fetch_add(1,std::memory_order_relaxed);const auto cd=c[6]-c[0];
        timingAgg.sumCreation.fetch_add(cd,std::memory_order_relaxed);raise_max(timingAgg.maxCreation,cd);return;
    }
    if(c[1]<=0 || c[5]<=0 || c[6]<c[5] || c[5]<c[1]) return;
    const auto submit=c[6]-c[5],encode=c[6]-c[1];
    timingAgg.jobs.fetch_add(1,std::memory_order_relaxed);
    timingAgg.sumSubmit.fetch_add(submit,std::memory_order_relaxed);timingAgg.sumEncode.fetch_add(encode,std::memory_order_relaxed);
    raise_max(timingAgg.maxSubmit,submit);raise_max(timingAgg.maxEncode,encode);
    if(const auto admit=admit_stamp(deadline-1); admit>0 && c[6]>=admit) {
        const auto total=c[6]-admit;
        timingAgg.admitJobs.fetch_add(1,std::memory_order_relaxed);timingAgg.sumAdmit.fetch_add(total,std::memory_order_relaxed);
        raise_max(timingAgg.maxAdmit,total);
    }
    if(const auto check=admit_stamp(deadline); check>0 && c[6]>check) timingAgg.late.fetch_add(1,std::memory_order_relaxed);
    if(c[0]>0 && q[1]>=c[0] && c[1]>=q[1] && c[5]>=c[1]) {
        timingAgg.stageJobs.fetch_add(1,std::memory_order_relaxed);
        const auto prod=q[1]-c[0];
        timingAgg.sumProd.fetch_add(prod,std::memory_order_relaxed);raise_max(timingAgg.maxProd,prod);
        timingAgg.sumPrep.fetch_add(c[1]-q[1],std::memory_order_relaxed);
        timingAgg.sumRec.fetch_add(c[5]-c[1],std::memory_order_relaxed);
        timingAgg.sumGpu.fetch_add(c[6]-c[5],std::memory_order_relaxed);
        {
            const auto ad=admit_stamp(deadline-1);const auto chk=admit_stamp(deadline);
            auto& e=jobTrace[jobTraceHead.fetch_add(1,std::memory_order_relaxed)&15];
            e.deadline.store(deadline);e.queue.store(ad>0&&c[0]>=ad?c[0]-ad:0);e.run.store(c[6]-c[0]);
            e.total.store(ad>0&&c[6]>=ad?c[6]-ad:0);e.late.store(chk>0&&c[6]>chk?1:0);
        }
        if(const auto admit2=admit_stamp(deadline-1); admit2>0 && c[0]>=admit2) {
            const auto qd=c[0]-admit2;
            timingAgg.queueJobs.fetch_add(1,std::memory_order_relaxed);timingAgg.sumQueue.fetch_add(qd,std::memory_order_relaxed);raise_max(timingAgg.maxQueue,qd);
        }
    }
}
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
struct CreateReceipt { uint32_t code=0;bool handle=false; };
struct CreateContext { Driver* driver;CreateReceipt& receipt; };
thread_local CreateContext* currentCreate=nullptr;
struct CreateScope {
    CreateContext context;
    CreateContext* previous=currentCreate;
    CreateScope(Driver* driver,CreateReceipt& receipt):context{driver,receipt} {currentCreate=&context;}
    ~CreateScope(){currentCreate=previous;}
    CreateScope(const CreateScope&)=delete;
    CreateScope& operator=(const CreateScope&)=delete;
};
struct Driver {
    lifecycle::VendorTrace* trace=nullptr;
    ComPtr<ID3D12Device> device;
    HMODULE module=nullptr;
    runtime::Dispatch dispatch;
    decltype(dispatch.create) createRaw=nullptr;
    HANDLE pinnedFile=INVALID_HANDLE_VALUE;
    bool initUnreturned=false,initFailed=false;
    std::function<lifecycle::VendorResult()> releaseBoundary; // CPU external ABI boundary only.
    std::function<NVSDK_NGX_Result(unsigned&)> shutdownBoundary;
    // _nvngx core export, not the public SDK/model one-argument wrapper.
    using Shutdown=NVSDK_NGX_Result (*)(ID3D12Device*,unsigned int&);
    // A co-resident NGX interposer (OptiScaler class) turns this process-wide Shutdown1 into an abort of its own consumers
    // (measured: runtime filter removal beside injected winmm.dll + dxgi.dll). When the real export is interposed we skip only
    // that call; the module release and device release below stay normal (the verified diagnostic configuration).
    static constexpr uint32_t shutdownSkipped=0x534B4950u; // 'SKIP': receipt error marker, a skipped Shutdown1 is not proof of retirement
    Shutdown shutdown=nullptr;
    bool initialized=false;
    uint32_t shutdownResult=0,shutdownRemaining=0,releaseError=0;
    bool shutdownCalled=false;
    ~Driver() {
        if(initialized || initUnreturned || initFailed) {
            OutputDebugStringA("NR driver initialization retained without successful worker shutdown");
            (void)device.Detach(); module=nullptr;pinnedFile=INVALID_HANDLE_VALUE;
        }
        if(module)(void)release_module();
        if(pinnedFile!=INVALID_HANDLE_VALUE)CloseHandle(pinnedFile);
    }
    lifecycle::VendorResult release_module() {
        return trace->call(lifecycle::VendorStage::CoreLibraryRelease,
            {reinterpret_cast<uintptr_t>(module),reinterpret_cast<uintptr_t>(&FreeLibrary),GetCurrentThreadId()},[&] {
                if(releaseBoundary)return releaseBoundary();
                const auto result=FreeLibrary(module);
                return lifecycle::VendorResult{static_cast<uint32_t>(result),0,result?0u:GetLastError()};
            });
    }
    bool close() {
        releaseError=0;
        if(initUnreturned || initFailed)return false;
        unsigned remaining=0;
        if(initialized) {
            shutdownCalled=true;
            const auto receipt=trace->call(lifecycle::VendorStage::CoreShutdown,
                {reinterpret_cast<uintptr_t>(module),reinterpret_cast<uintptr_t>(shutdown),GetCurrentThreadId()},[&] {
                    if(!shutdownBoundary && entry_interposed(module,reinterpret_cast<const void*>(shutdown))) {
                        OutputDebugStringA("NR driver: Shutdown1 skipped (export interposed); retirement is not proven\n");
                        remaining=0;
                        return lifecycle::VendorResult{static_cast<uint32_t>(NVSDK_NGX_Result_Success),0,shutdownSkipped};
                    }
                    const auto code=shutdownBoundary?shutdownBoundary(remaining):shutdown(device.Get(),remaining);
                    return lifecycle::VendorResult{static_cast<uint32_t>(code),remaining,0};
                });
            shutdownResult=receipt.result;
            shutdownRemaining=remaining;
            if(shutdownResult!=static_cast<uint32_t>(NVSDK_NGX_Result_Success) || remaining!=0)return false;
        }
        initialized=false;
        if(module) {
            const auto receipt=release_module();
            if(!receipt.result){releaseError=receipt.error;return false;}
        }
        module=nullptr;
        if(pinnedFile!=INVALID_HANDLE_VALUE){CloseHandle(pinnedFile);pinnedFile=INVALID_HANDLE_VALUE;}
        device.Reset();return true;
    }
    static NVSDK_NGX_Result create(ID3D12GraphicsCommandList* commands,NVSDK_NGX_Feature feature,
        NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** handle) {
        require(currentCreate && currentCreate->driver,"private driver create call owner");
        auto& context=*currentCreate;
        const auto code=context.driver->createRaw(commands,feature,params,handle);
        context.receipt={static_cast<uint32_t>(code),handle && *handle};
        return code;
    }
    void open(ID3D12Device* input,const Config& config,const runtime::DriverBinding& expected,
        const std::filesystem::path& canonicalData,lifecycle::VendorTrace& ownerTrace) {
        auto* result=this;result->device=input;
        result->trace=&ownerTrace;
        require(config.driver.is_absolute() && std::filesystem::canonical(config.driver)==config.driver.lexically_normal(),
            "canonical owned driver path required");
        result->pinnedFile=CreateFileW(config.driver.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(result->pinnedFile!=INVALID_HANDLE_VALUE,"retained verified CORE file");
        const auto binding=runtime::resolve_driver();
        require(binding.signatureVerified&&binding.identity.canonicalPath==config.driver &&
            binding.identity.canonicalPath==expected.identity.canonicalPath &&
            binding.identity.sha256==expected.identity.sha256,
            "fresh active NVIDIA CORE binding required");
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
        const auto directory=canonicalData.wstring();
        const wchar_t* paths[]{directory.c_str()};
        NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path=paths; info.PathListInfo.Length=1;
        result->initUnreturned=true;
        const auto receipt=ownerTrace.call(lifecycle::VendorStage::CoreInit,
            {reinterpret_cast<uintptr_t>(result->module),reinterpret_cast<uintptr_t>(init),GetCurrentThreadId()},[&] {
                return lifecycle::VendorResult{static_cast<uint32_t>(init(0x24480451ull,canonicalData.c_str(),input,NVSDK_NGX_Version_API,&info)),0,0};
            });
        result->initUnreturned=false;
        const auto code=static_cast<NVSDK_NGX_Result>(receipt.result);
        if(code!=NVSDK_NGX_Result_Success) {
            result->initFailed=true;
            throw std::runtime_error("private NGX driver Init_Ext result="+std::to_string(static_cast<uint32_t>(code)));
        }
        result->initialized=true;
    }
};
#include "nr_model_session.inl"
}
bool observation_destination(ID3D12Resource* source,ID3D12Resource* target,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint) {
    if(!source||!target)return false;
    const auto a=source->GetDesc(),b=target->GetDesc();
    if(b.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)
        return a.Dimension==b.Dimension&&a.Width==b.Width&&a.Height==b.Height&&a.Format==b.Format&&
            a.SampleDesc.Count==b.SampleDesc.Count;
    if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||a.MipLevels!=1||a.DepthOrArraySize!=1||
        a.SampleDesc.Count!=1||footprint.Offset!=0)return false;
    ComPtr<ID3D12Device> device;
    if(FAILED(source->GetDevice(IID_PPV_ARGS(&device))))return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT expected{};UINT rows=0;UINT64 rowBytes=0;
    device->GetCopyableFootprints(&a,0,1,0,&expected,&rows,&rowBytes,nullptr);
    const auto& x=footprint.Footprint;const auto& y=expected.Footprint;
    return rows&&x.Format==y.Format&&x.Width==y.Width&&x.Height==y.Height&&x.Depth==y.Depth&&
        x.RowPitch==y.RowPitch&&b.Width>=static_cast<uint64_t>(rows-1)*x.RowPitch+rowBytes;
}
bool copy_observation(ID3D12GraphicsCommandList* commands,Observation& observation,const JobKey& key,
    const std::array<ID3D12Resource*,Observation::Count>& sources) {
    if(!commands||!observation.key||*observation.key!=key)return false;
    for(size_t i=0;i<sources.size();++i)if(sources[i]&&!observation_destination(sources[i],
        observation.images[i].Get(),observation.imageFootprints[i]))return false;
    for(size_t i=0;i<sources.size();++i)if(sources[i]) {
        auto* target=observation.images[i].Get();
        barrier(commands,sources[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(commands,target,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        if(target->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) {
            D3D12_TEXTURE_COPY_LOCATION from{},to{};
            from.pResource=sources[i];from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint=observation.imageFootprints[i];
            commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        }else commands->CopyResource(target,sources[i]);
        barrier(commands,target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        barrier(commands,sources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        observation.stages|=1ull<<i;
    }
    return true;
}
Frame::Frame(ID3D12Resource* raw,ID3D12Resource* flow,std::shared_ptr<void> lease,NV_OF_FENCE_POINT ready)
    :original(raw),analysis(flow),producerLease(ready.fence),graphicsLease(std::move(lease)),producer(ready) {
    require(raw && flow && raw!=flow && graphicsLease && ready.fence && ready.value,"immutable model frame required");
}
Job::Job(std::shared_ptr<const Frame> input,const JobContract& sample,ColorExposure value,Fault control)
    :frame(std::move(input)),contract(sample),exposure(value),fault(control),originalMarker(sample.key),
      observed(CreateEventW(nullptr,TRUE,FALSE,nullptr)),gpu(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    require(frame && observed && gpu,"model job and exact completion events");
}
Job::~Job() { if(observed) CloseHandle(observed); if(gpu) CloseHandle(gpu); }
struct Job::Resources {
    const std::shared_ptr<interop::Bridge> bridge;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> creation;
    std::array<ComPtr<ID3D12Resource>,5> colors;
    std::array<ComPtr<ID3D12Resource>,4> guideObservations;
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
std::array<ID3D12Resource*,4> Job::guide_observation_resources() const {
    std::array<ID3D12Resource*,4> result{};
    if(resources) for(size_t i=0;i<result.size();++i) result[i]=resources->guideObservations[i].Get();
    return result;
}
std::array<uintptr_t,3> Job::descriptor_heaps() const {
    return resources?std::array<uintptr_t,3>{reinterpret_cast<uintptr_t>(resources->codecHeap.Get()),
        reinterpret_cast<uintptr_t>(resources->prepareHeap.Get()),reinterpret_cast<uintptr_t>(resources->restoreHeap.Get())}:
        std::array<uintptr_t,3>{};
}
namespace {
void observe_completed(Job& job) {
    auto* o=job.observation;
    if(!o)return;
    o->refused=o->refused||!o->key||*o->key!=job.contract.key;
    o->created=job.created;o->evaluated=job.evaluated;o->composed=job.composed;
    o->submitted=job.submitted;o->grid=job.guideGrid;o->code=job.code;
    o->invalid=job.invalid;o->module=job.runtimeModule;
    o->chronology=job.chronology;o->timing=job.qaTiming;
    o->reset=!job.flow||job.flow->reset;
    o->referenceValid=job.referenceValid;
    if(job.guideReferenceKey)o->reference.emplace(*job.guideReferenceKey);
    if(job.resources) {
        o->completionFence=job.resources->complete;
        o->completionValue=1; // This actual job's separately created completion fence.
    }
    job.observation=nullptr;
    o->finished.store(true,std::memory_order_release);
    SetEvent(o->observed);
}
void finish_completed(Job& job) {
    auto& r=*job.resources;
    if(!job.callbackArrived.load() ||
        !original_completion_allowed(job.fault,job.originalMarker.state(),r.complete->GetCompletedValue(),
            job.originalSignalResult.load()))return;
    bool expected=false;
    if(!job.completionClaimed.compare_exchange_strong(expected,true))return;
    job.chronology[6]=stamp();
    if(r.statusRecorded) {
        void* bytes=nullptr; const D3D12_RANGE range{0,4};
        if(FAILED(r.statusRead->Map(0,&range,&bytes))) { job.code=0xe001; job.invalid=UINT32_MAX; }
        else { memcpy(&job.invalid,bytes,4); const D3D12_RANGE empty{0,0}; r.statusRead->Unmap(0,&empty); }
    }
    job.eligible=qa_publication_allowed(job.qaFailure.load(std::memory_order_acquire)) &&
        job.evaluated && job.composed && job.submitted && !job.invalid &&
        r.submission->state()==runtime::Completion::Complete;
    { // diagnostic: why a completed job is or is not publishable
        const bool hist=job.flow.has_value() && !job.flow->reset;
        const bool complete=r.submission->state()==runtime::Completion::Complete;
        const bool qaOk=qa_publication_allowed(job.qaFailure.load(std::memory_order_acquire));
        auto& e=eligTrace[eligHead.fetch_add(1,std::memory_order_relaxed)&15];
        e.deadline.store(job.contract.deadlineTick);e.invalid.store(job.invalid);e.code.store(job.code);
        e.flags.store((hist?1u:0u)|(job.eligible?2u:0u)|(job.evaluated?4u:0u)|(job.submitted?8u:0u)|(complete?16u:0u)|(qaOk?32u:0u)|(job.flow.has_value()?64u:0u));
        eligAgg.n.fetch_add(1);
        if(!job.eligible){eligAgg.bad.fetch_add(1);(hist?eligAgg.badHist:eligAgg.badReset).fetch_add(1);eligAgg.orMask.fetch_or(job.invalid);
            if(!job.evaluated)eligAgg.noEval.fetch_add(1);if(!job.submitted)eligAgg.noSubmit.fetch_add(1);
            if(!complete)eligAgg.incomplete.fetch_add(1);if(!qaOk)eligAgg.qa.fetch_add(1);}
    }
    job.chronology[7]=stamp();record_timing(job.chronology,job.contract.deadlineTick,job.qaTiming);
    job.ready.store(true,std::memory_order_release);
    observe_completed(job);
    SetEvent(job.observed);
}
void CALLBACK completed(void* opaque,BOOLEAN) {
    std::unique_ptr<std::shared_ptr<Job>> retained(static_cast<std::shared_ptr<Job>*>(opaque));
    auto& job=**retained;
    // Seq-cst handoff with marker state prevents a lost completion when the
    // original fence callback arrives before Signal has returned its HRESULT.
    job.callbackArrived.store(true);
    finish_completed(job);
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
    std::atomic<std::shared_ptr<const lifecycle::VendorTrace>> traceView;
    const std::shared_ptr<interop::Context> context;
    const Config config;
    const uint64_t instance;
    std::thread::id worker;
    std::shared_ptr<SdkSession> session; // Exactly one explicit owner lease.
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
    std::atomic<std::shared_ptr<const GuidePreparation>> preparation;
    std::atomic<bool> closed{false};
    std::atomic<uint64_t> closeVersion{0};
    std::array<std::atomic<uint32_t>,18> closeValues{};
    std::array<std::atomic<uint64_t>,5> closePending{};
    CreateReceipt creationReceipt;
    uint64_t epoch=0;
    uint64_t creationEpoch=0;
    uint64_t generation=0;
    bool reset=true;
    const char* lastResetSetter="initial-reset";
    // N23 worker diagnostics (no behavior change).
    std::atomic<uint64_t> diagOk{0},diagFail{0},diagExc{0};std::atomic<uint32_t> diagSettle{0},diagLastCode{0},diagFirstN{0};
    std::array<std::atomic<uint64_t>,8> diagFirst{};std::array<char,160> diagWhat{};std::atomic<bool> diagWhatSet{false};
    void diag_fail(uint32_t code) {
        diagFail.fetch_add(1);diagLastCode.store(code);const auto n=diagFirstN.load();
        if(n<diagFirst.size()) {
            uint64_t v=code&0xffff;v|=uint64_t(diagSettle.load()&3)<<16;if(guides)v|=uint64_t(guides->state_bits()&7)<<18;
            if(pending){v|=1ull<<21;if(pending->ready.load())v|=1ull<<22;if(pending->callbackArrived.load())v|=1ull<<23;
                if(pending->resources&&pending->resources->complete)v|=uint64_t(std::min<uint64_t>(pending->resources->complete->GetCompletedValue(),3))<<24;}
            if(pendingGuide)v|=1ull<<26;diagFirst[n].store(v);diagFirstN.store(n+1);
        }
        diagSettle.store(0);
    }
    Impl(std::shared_ptr<interop::Context> value,Config input,uint64_t id)
        :context(std::move(value)),config(std::move(input)),instance(id) {
        require(context && id && !config.codec.empty() && !config.color.empty() && !config.guides.empty(),
            "private context and actual compiled model shaders required");
    }
    bool settle() {
        if(!pending) return true;
        if(!pending->ready.load(std::memory_order_acquire)) {diagSettle.store(1);return false;}
        if(pendingGuide && !guides->collect(*pendingGuide,{pending->resources->complete.Get(),1})) {diagSettle.store(2);return false;}
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
        // A registered wait is a live owner: publish it before any later failure,
        // so settlement and the close receipt hold it until its callback drains.
        pending=job;
    }
    void signal(const std::shared_ptr<Job>& job) {
        pending=job;
        const auto result=completion_signal(job->fault,[&] {
            const auto actual=context->queue()->Signal(job->resources->complete.Get(),1);
            job->originalSignalResult.store(actual);
            return actual;
        });
        if(FAILED(result) && job->fault!=Fault::None) {
            job->qaFailure.store(0xe01a,std::memory_order_release);
            // Only the helper's omission can hold. Prior real errors and a
            // failed actual Signal never become releaseable by this selector.
            job->originalMarker.hold(job->fault,job->submitted,
                (job->created || (job->evaluated && job->composed)) &&
                context->device()->GetDeviceRemovedReason()==S_OK &&
                job->resources->complete->GetCompletedValue()==0 &&
                (job->resources->submission->state()==runtime::Completion::Submitted ||
                    job->resources->submission->state()==runtime::Completion::Complete));
        }
        if(job->callbackArrived.load())finish_completed(*job);
        checked(result,"model retained completion signal");
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
bool Processor::request_original_marker(const std::shared_ptr<Job>& job,const JobKey& key) {
    return job && job->fault==Fault::CompletionSignal && key==job->contract.key &&
        key.frame.instanceId==impl_->instance && job->originalMarker.request(key);
}
void Processor::worker_service() noexcept {
    auto& p=*impl_;
    if(p.worker!=std::this_thread::get_id() || !p.pending)return;
    const auto job=p.pending; // Only its existing worker reads pending.
    job->originalMarker.consume(job->contract.key,[&] {
        return job->submitted && job->qaFailure.load(std::memory_order_acquire)==0xe01a &&
            p.context->device()->GetDeviceRemovedReason()==S_OK &&
            job->resources && job->resources->complete->GetCompletedValue()==0 &&
            (job->resources->submission->state()==runtime::Completion::Submitted ||
                job->resources->submission->state()==runtime::Completion::Complete);
    },[&](uint64_t target) {
        const auto actual=p.context->queue()->Signal(job->resources->complete.Get(),target);
        job->originalSignalResult.store(actual);
        return actual;
    });
    // Only a genuinely delivered original callback can finish here. No event,
    // ready flag, or substitute runtime-submission completion is invented.
    if(job->callbackArrived.load())finish_completed(*job);
}
bool Processor::worker_stop_complete() const noexcept { return impl_->closed.load(std::memory_order_acquire); }
lifecycle::ModelClose Processor::close_diagnostics() const
{
    const auto& p=*impl_;const auto before=p.closeVersion.load(std::memory_order_acquire);
    lifecycle::ModelClose d;
    if(const auto trace=p.traceView.load(std::memory_order_acquire))d.vendor=trace->snapshot();
    d.settlement=static_cast<lifecycle::CloseResult>(p.closeValues[0].load());
    d.feature=static_cast<lifecycle::CloseResult>(p.closeValues[1].load());
    d.ofa=static_cast<lifecycle::CloseResult>(p.closeValues[2].load());
    d.module=static_cast<lifecycle::CloseResult>(p.closeValues[3].load());
    d.core=static_cast<lifecycle::CloseResult>(p.closeValues[4].load());
    d.coreResult=p.closeValues[5].load();d.coreRemaining=p.closeValues[6].load();
    d.moduleError=p.closeValues[7].load();d.exception=p.closeValues[8].load();
    d.coreCalled=p.closeValues[9].load()!=0;
    d.pendingSerial=p.closePending[0].load();d.completionRequired=p.closePending[1].load();d.completionDone=p.closePending[2].load();
    d.callbackRetained=p.closeValues[10].load()!=0;d.guidePending=p.closeValues[11].load()!=0;
    d.moduleRequired=p.closeValues[12].load()!=0;d.modulePresent=p.closeValues[13].load()!=0;
    d.sdk=static_cast<lifecycle::SdkDisposition>(p.closeValues[14].load());
    d.moduleReferenceReleased=p.closeValues[15].load()!=0;d.iatRestored=p.closeValues[16].load()!=0;
    d.coreReferenceReleased=p.closeValues[17].load()!=0;
    d.sessionId=p.closePending[3].load();d.ownersAtDetachment=p.closePending[4].load();
    if(!before && d.vendor.owner) {d.sdk=lifecycle::SdkDisposition::Owned;d.sessionId=d.vendor.owner;}
    const auto after=p.closeVersion.load(std::memory_order_acquire);
    d.attempts=(after+1)/2;d.coherent=before==after && !(after&1);
    return d;
}
bool pin_module_for_export() noexcept {
    static const bool pinned=[] {
        HMODULE module=nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&__ImageBase),&module)!=FALSE;
    }();
    return pinned;
}
std::shared_ptr<QaSignal> make_qa_signal(){
    if(!pin_module_for_export())OutputDebugStringA("NR QA signal module pin failed");
    return std::make_shared<ModuleSignal>();
}
uint64_t qa_signal_live_count(){return signalLive.load(std::memory_order_acquire);}
uint64_t Processor::applied() const { return impl_->appliedCount.load(std::memory_order_acquire); }
std::shared_ptr<const GuidePreparation> Processor::guide_preparation() const {
    return impl_->preparation.load(std::memory_order_acquire);
}
bool Processor::publish(const std::shared_ptr<Job>& job,const Selection& selected,const interop::Ownership& physical) {
    if(GetCurrentThreadId()!=impl_->context->graphics_thread() || !job || !job->ready.load(std::memory_order_acquire) ||
        !qa_publication_allowed(job->qaFailure.load(std::memory_order_acquire)) || !job->eligible ||
        selected.kind!=OutputKind::DelayedNr || !selected.job || *selected.job!=job->contract.key ||
        !selected.frame || *selected.frame!=job->contract.key.frame || selected.canonicalTick!=job->contract.deadlineTick ||
        !interop::publishable(physical,job->contract.key) || physical.consumerOpen) return false;
    bool expected=false;
    if(!job->published.compare_exchange_strong(expected,true)) return false;
    impl_->appliedCount.fetch_add(1,std::memory_order_relaxed); return true;
}
std::string Processor::diag() const {
    const auto& p=*impl_;char b[64];
    std::string s=" mdl ok="+std::to_string(p.diagOk.load())+" fail="+std::to_string(p.diagFail.load())+" exc="+std::to_string(p.diagExc.load());
    std::snprintf(b,sizeof b," last=%x",p.diagLastCode.load());s+=b;
    for(uint32_t i=0;i<p.diagFirstN.load()&&i<p.diagFirst.size();++i){std::snprintf(b,sizeof b," f%u=%llx",i,(unsigned long long)p.diagFirst[i].load());s+=b;}
    if(p.diagWhatSet.load(std::memory_order_acquire)){s+=" what=";s+=p.diagWhat.data();}
    return s;
}
pipeline::Processed Processor::process(const pipeline::Work& work,std::stop_token stop) {
    auto& p=*impl_;
    if(p.worker==std::thread::id{}) p.worker=std::this_thread::get_id();
    require(p.worker==std::this_thread::get_id(),"single scheduler worker owns model");
    auto job=std::static_pointer_cast<Job>(work.binding.processorLease);
    require(job && job->contract==work.job && work.raw && work.job.input==work.raw->input && work.binding.transport &&
        work.binding.ticket.key==work.job.key && work.binding.capturePin &&
        work.binding.capturePin->ticket==work.binding.ticket,"exact captured model binding");
    job->chronology[0]=stamp();if(p.config.qaObservations||job->observation)job->qaTiming[0]=stamp();
    auto fail=[&](uint32_t code) {
        job->code=code; p.reset=true; p.lastResetSetter="presubmit-failure";
        p.diag_fail(code);
        if(job->fault!=Fault::None)job->qaFailure.store(code,std::memory_order_release);
        require(work.binding.transport->discard_recording(work.binding.ticket),"model presubmit discard");
        job->ready.store(true,std::memory_order_release); observe_completed(*job);SetEvent(job->observed);
        return pipeline::Processed{work.job,false,{}};
    };
    try {
    if(stop.stop_requested() || !p.settle()) return fail(0xe002);
    job->epoch=++p.epoch; job->creationEpoch=p.creationEpoch;
    if(job->fault==Fault::Capability) return fail(0xe003); // Owned admission control, no feature/compose.
    const auto producer=job->frame->producer;
    const auto done=producer.fence->GetCompletedValue();
    const auto producerCode=producer_return(job->fault,producer.value,done,[&] {
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"producer event");
        const auto result=producer.fence->SetEventOnCompletion(producer.value,event);
        const auto wait=SUCCEEDED(result)?WaitForSingleObject(event,15000):WAIT_FAILED;
        CloseHandle(event);
        return wait==WAIT_OBJECT_0 && producer.fence->GetCompletedValue()!=UINT64_MAX;
    });
    if(producerCode)return fail(producerCode);
    const auto& d=work.job.input.descriptor;
    job->qaTiming[1]=stamp();
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
        p.reset=true; p.lastResetSetter="pass-retired";
    }
    if(!p.session) {
        if(p.config.qaObservations||job->observation)job->qaTiming[2]=stamp();
        const auto binding=runtime::resolve_driver();
        const auto acquisition=sessions().acquire(session_key(device,p.config,binding));
        if(!acquisition.session)return fail(0xe020); // Initializing/retiring/conflicting owner; never render waits.
        p.session=acquisition.session;p.driver=p.session->driver;
        p.traceView.store(std::shared_ptr<const lifecycle::VendorTrace>(p.session,&p.session->trace),
            std::memory_order_release); // Observation only, not another SDK lease.
        if(acquisition.initialize) {
            try {
                HMODULE existing=nullptr;
                require(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    p.config.runtime.canonicalPath.c_str(),&existing),"pre-existing foreign NR image");
                p.driver->open(device,p.config,binding,p.session->key.data,p.session->trace);
                if(job->fault==Fault::AfterCoreInit) {
                    sessions().retain(p.session);return fail(0xe019);
                }
                p.session->module=runtime::Module::open(p.config.runtime,p.driver->dispatch,p.driver,device,
                    p.session->key.data,p.session->key.route,&p.session->trace);
                require(p.session->module!=nullptr,"selected runtime module admission");
                sessions().publish(p.session);
            }catch(...) {sessions().retain(p.session);throw;}
        }
        p.module=p.session->module;
    }
    if(p.session->state.load(std::memory_order_acquire)!=SessionState::Ready)return fail(0xe020);
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
        if(p.config.qaObservations||job->observation)job->qaTiming[3]=stamp();
        p.creationReceipt={};
        const CreateScope createScope(p.driver.get(),p.creationReceipt);
        const auto code=p.pass->prepare(work.job.input.settings,size,work.job.key.historyEpoch,recording(job,r.creation.Get()));
        if(p.config.qaObservations||job->observation)job->qaTiming[4]=stamp();
        job->driverCreateCode=p.creationReceipt.code;job->driverHandle=p.creationReceipt.handle;
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
    job->driverCreateCode=p.creationReceipt.code;job->driverHandle=p.creationReceipt.handle;
    HMODULE image=nullptr;
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,p.config.runtime.canonicalPath.c_str(),&image),
        "live selected Feature18 module");
    job->runtimeModule=reinterpret_cast<uintptr_t>(image);
    std::array<wchar_t,32768> path{};
    const auto count=GetModuleFileNameW(image,path.data(),static_cast<DWORD>(path.size()));
    require(count && count<path.size(),"live selected module path"); job->modulePath.assign(path.data(),count);
    job->resetDiagnostic.emplace(decltype(job->resetDiagnostic)::value_type{true,p.reset,p.previousKey,p.lastResetSetter});
    const bool reset=p.reset || !p.previousKey || p.previousKey->historyEpoch!=work.job.key.historyEpoch ||
        p.previousKey->frame.contentEpoch!=work.job.key.frame.contentEpoch ||
        p.previousKey->frame.ingressSequence+1!=work.job.key.frame.ingressSequence;
    if(!p.guides) p.guides=std::make_unique<D3D12Guides>(device,p.context->queue(),d.allocation,p.config.guides,p.config.frameIntervalNs);
    if(p.config.qaPreparation && !p.preparation.load(std::memory_order_relaxed))
        p.preparation.store(std::make_shared<const GuidePreparation>(GuidePreparation{
            work.job.key,p.guides->observation_descriptors(),p.guides->grid_size()}),std::memory_order_release);
    const FlowImage current{work.job.key,job->frame->analysis.Get(),producer};
    std::optional<FlowImage> previous;
    if(!reset) previous.emplace(FlowImage{*p.previousKey,p.previousFrame->analysis.Get(),p.previousFrame->producer});
    const auto flow=p.guides->submit({current,previous?&*previous:nullptr,d.allocation,d.active,reset});
    if(flow!=FlowProblem::None) return fail(0xe100+static_cast<uint32_t>(flow));
    const auto* guide=p.guides->result(); require(guide && guide->key==work.job.key,"exact native guides");
    job->flow.emplace(*guide);
    job->guideGrid=p.guides->grid_size();
        appliedPerf.store(static_cast<uint32_t>(p.guides->perf_level()));appliedGrid.store(p.guides->grid_size());appliedHint.store(p.guides->hint_grid_size());
    if(previous) job->guideReferenceKey.emplace(previous->key);
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
        p.reset=true; p.lastResetSetter="core-record-refused"; p.signal(job); return {work.job,false,{}};
    }
    barrier(list,r.colors[4].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(list,view->output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER order{}; order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; order.UAV.pResource=r.status.Get();
    list->ResourceBarrier(1,&order);
    job->referenceValid=!reset&&previous.has_value();
    require(r.color->record(list,r.restoreHeap.Get(),*plan,job->frame->original.Get(),r.colors[4].Get(),
        view->output,r.status.Get(),true,job->referenceValid),"actual source-relative color restore");
    barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(r.statusRead.Get(),0,r.status.Get(),0,4); r.statusRecorded=true;
    barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,job->frame->original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,guide->depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,guide->motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,view->output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    for(size_t i=0;i<r.colors.size();++i) barrier(list,r.colors[i].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    if(auto* o=job->observation) {
        const std::array<ID3D12Resource*,Observation::Count> sources{
            r.colors[0].Get(),r.colors[1].Get(),r.colors[2].Get(),r.colors[3].Get(),r.colors[4].Get(),
            job->frame->analysis.Get(),previous?previous->analysis:job->frame->analysis.Get(),
            guide->motion,guide->hardwareVectors,guide->cost,view->output};
        o->refused=!copy_observation(list,*o,work.job.key,sources);
    }
    if(p.config.qaObservations) {
        const std::array<ID3D12Resource*,4> inputs{
            previous?previous->analysis:job->frame->analysis.Get(),guide->motion,guide->hardwareVectors,guide->cost};
        for(size_t i=0;i<inputs.size();++i) {
            if(!inputs[i]) continue; // Reset has no native vectors/cost, never synthetic normal flow.
            auto desc=inputs[i]->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES allocation{};allocation.Type=D3D12_HEAP_TYPE_DEFAULT;
            checked(device->CreateCommittedResource(&allocation,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r.guideObservations[i])),"bounded QA guide snapshot");
            barrier(list,inputs[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyResource(r.guideObservations[i].Get(),inputs[i]);
            barrier(list,inputs[i],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
            barrier(list,r.guideObservations[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        }
    }
    job->submitted=work.binding.transport->submit_worker(work.binding.ticket);
    job->chronology[4]=stamp();
    if(job->submitted) job->submitted=r.submission->submitted(p.context->queue());
    job->chronology[5]=stamp();
    p.reset=!job->submitted; p.lastResetSetter="submission-result";
    p.previousKey.emplace(work.job.key); p.previousFrame=job->frame;
    p.signal(job);
    p.diagOk.fetch_add(1);
    return {work.job,job->submitted,job};
    } catch(const std::exception& error) {
        job->diagnostic=error.what(); job->code=0xefff; p.reset=true; p.lastResetSetter="process-exception";
        p.diagExc.fetch_add(1);if(!p.diagWhatSet.load()){{const char* m=error.what();size_t i=0;for(;m[i]&&i+1<p.diagWhat.size();++i)p.diagWhat[i]=m[i];p.diagWhat[i]=0;}p.diagWhatSet.store(true,std::memory_order_release);}p.diag_fail(0xefff);
        // No recording/resources means no feature submission occurred. All other
        // failures retain their exact resource owners; never manufacture completion.
        if(!job->resources && work.binding.transport->discard_recording(work.binding.ticket)) {
            job->ready.store(true,std::memory_order_release);observe_completed(*job);SetEvent(job->observed);
        }
        return {work.job,false,{}};
    }
}
void Processor::worker_stopping() noexcept {
    auto& p=*impl_;
    if(p.closed.load(std::memory_order_acquire))return;
    p.closeVersion.fetch_add(1,std::memory_order_acq_rel);
    struct Publish {std::atomic<uint64_t>& version;~Publish(){version.fetch_add(1,std::memory_order_release);}} publish{p.closeVersion};
    lifecycle::ModelClose proof;
    if(p.session){proof.sdk=lifecycle::SdkDisposition::Owned;proof.sessionId=p.session->id;}
    bool complete=false;
    try {
        if(p.worker==std::thread::id{})p.worker=std::this_thread::get_id();
        require(p.worker==std::this_thread::get_id(),"model cleanup on its worker");
        const auto step=[&](size_t index) {
            switch(index) {
            case 0:
                proof.guidePending=p.pendingGuide.has_value();
                if(p.pending) {
                    const auto fence=p.pending->completion();
                    proof.pendingSerial=p.pending->contract.key.submissionSerial;
                    proof.completionRequired=fence.value;
                    proof.completionDone=fence.fence?fence.fence->GetCompletedValue():0;
                    proof.callbackRetained=p.pending->resources && p.pending->resources->wait;
                }
                if(!p.settle())return false;
                proof.pendingSerial=proof.completionRequired=proof.completionDone=0;
                proof.callbackRetained=proof.guidePending=false;
                return true;
            case 1:
                if(p.pass && !p.pass->release())return false;
                p.pass.reset();p.previousFrame.reset();p.previousKey.reset();
                return true;
            case 2:
                if(p.guides && !p.guides->close())return false;
                p.guides.reset();
                return true;
            case 3:
                return !p.session || close_session_module(sessions(),p.session,proof);
            case 4:
                return !p.session || close_session_core(sessions(),p.session,proof);
            default: throw std::logic_error("unknown model close stage");
            }
        };
        struct Actions { const decltype(step)& call;bool step(size_t i) {return call(i);} } actions{step};
        complete=lifecycle::close_model_steps(proof,actions);
    } catch(...) {
        proof.exception=GetLastError()?GetLastError():1u;
        OutputDebugStringA("NR model worker shutdown retained exact owner");
    }
    const std::array values{static_cast<uint32_t>(proof.settlement),static_cast<uint32_t>(proof.feature),
        static_cast<uint32_t>(proof.ofa),static_cast<uint32_t>(proof.module),static_cast<uint32_t>(proof.core),
        proof.coreResult,proof.coreRemaining,proof.moduleError,proof.exception,proof.coreCalled?1u:0u,
        proof.callbackRetained?1u:0u,proof.guidePending?1u:0u,proof.moduleRequired?1u:0u,proof.modulePresent?1u:0u,
        static_cast<uint32_t>(proof.sdk),proof.moduleReferenceReleased?1u:0u,proof.iatRestored?1u:0u,
        proof.coreReferenceReleased?1u:0u};
    for(size_t i=0;i<values.size();++i)p.closeValues[i].store(values[i]);
    p.closePending[0].store(proof.pendingSerial);p.closePending[1].store(proof.completionRequired);
    p.closePending[2].store(proof.completionDone);
    p.closePending[3].store(proof.sessionId);p.closePending[4].store(proof.ownersAtDetachment);
    if(complete) {
        p.module.reset();p.driver.reset();p.session.reset();p.closed.store(true,std::memory_order_release);
    }
}
int64_t qpc_now() { return stamp(); }
double qpc_ms(int64_t delta) {
    LARGE_INTEGER freq{};QueryPerformanceFrequency(&freq);
    return freq.QuadPart>0?1000.0*static_cast<double>(delta)/static_cast<double>(freq.QuadPart):0.0;
}
void note_admit(uint64_t tick,int64_t qpc) {
    const auto i=tick&255;
    tickRing.tick[i].store(0,std::memory_order_release);
    tickRing.qpc[i].store(qpc,std::memory_order_relaxed);
    tickRing.tick[i].store(tick,std::memory_order_release);
}
std::string take_job_trace_base() {
    const auto head=jobTraceHead.load(std::memory_order_relaxed);
    std::string out;
    const unsigned n=head<12?head:12;
    for(unsigned k=n;k>0;--k) {
        const auto& e=jobTrace[(head-k)&15];
        char buf[96];
        std::snprintf(buf,sizeof buf,"%s%llu:q%.0f/r%.0f/t%.0f%s",out.empty()?"":",",static_cast<unsigned long long>(e.deadline.load()%1000),
            qpc_ms(e.queue.load()),qpc_ms(e.run.load()),qpc_ms(e.total.load()),e.late.load()?"L":"");
        out+=buf;
    }
    return out;
}
std::string take_job_trace() {
    auto out=take_job_trace_base();
    out+=" | of perf"+std::to_string(appliedPerf.load())+" grid"+std::to_string(appliedGrid.load())+" hint"+std::to_string(appliedHint.load());
    char buf[200];
    std::snprintf(buf,sizeof buf," | elig n%u bad%u hist%u reset%u mask%x noEval%u noSub%u incomplete%u qa%u last(",
        eligAgg.n.exchange(0),eligAgg.bad.exchange(0),eligAgg.badHist.exchange(0),eligAgg.badReset.exchange(0),eligAgg.orMask.exchange(0),
        eligAgg.noEval.exchange(0),eligAgg.noSubmit.exchange(0),eligAgg.incomplete.exchange(0),eligAgg.qa.exchange(0));
    out+=buf;
    const auto head=eligHead.load(std::memory_order_relaxed);const unsigned n=head<8?head:8;
    for(unsigned k=n;k>0;--k) {
        const auto& e=eligTrace[(head-k)&15];
        std::snprintf(buf,sizeof buf,"%s%llu:i%x/c%x/f%x",k==n?"":",",static_cast<unsigned long long>(e.deadline.load()%1000),e.invalid.load(),e.code.load(),e.flags.load());
        out+=buf;
    }
    out+=")";
    return out;
}
JobTimingWindow take_job_timing() {
    LARGE_INTEGER freq{};QueryPerformanceFrequency(&freq);
    const double toMs=freq.QuadPart>0?1000.0/static_cast<double>(freq.QuadPart):0.0;
    JobTimingWindow w;
    w.jobs=timingAgg.jobs.exchange(0);
    const auto aj=timingAgg.admitJobs.exchange(0);const auto sa=timingAgg.sumAdmit.exchange(0);
    w.late=timingAgg.late.exchange(0);w.admitMaxMs=qpc_ms(timingAgg.maxAdmit.exchange(0));
    if(aj) w.admitMeanMs=qpc_ms(sa)/aj;
    const auto sj=timingAgg.stageJobs.exchange(0),qj=timingAgg.queueJobs.exchange(0);
    const auto sp=timingAgg.sumProd.exchange(0),spp=timingAgg.sumPrep.exchange(0),sr=timingAgg.sumRec.exchange(0),sg=timingAgg.sumGpu.exchange(0),sq=timingAgg.sumQueue.exchange(0);
    w.queueMaxMs=qpc_ms(timingAgg.maxQueue.exchange(0));w.prodMaxMs=qpc_ms(timingAgg.maxProd.exchange(0));
    if(sj){w.prodMeanMs=qpc_ms(sp)/sj;w.prepMeanMs=qpc_ms(spp)/sj;w.recMeanMs=qpc_ms(sr)/sj;w.gpuMeanMs=qpc_ms(sg)/sj;}
    if(qj) w.queueMeanMs=qpc_ms(sq)/qj;
    const auto cj=timingAgg.creationJobs.exchange(0);const auto scr=timingAgg.sumCreation.exchange(0);
    w.creationJobs=cj;w.creationMaxMs=qpc_ms(timingAgg.maxCreation.exchange(0));if(cj)w.creationMeanMs=qpc_ms(scr)/cj;
    const auto ss=timingAgg.sumSubmit.exchange(0),se=timingAgg.sumEncode.exchange(0);
    w.submitMaxMs=static_cast<double>(timingAgg.maxSubmit.exchange(0))*toMs;
    w.encodeMaxMs=static_cast<double>(timingAgg.maxEncode.exchange(0))*toMs;
    if(w.jobs){w.submitMeanMs=static_cast<double>(ss)*toMs/w.jobs;w.encodeMeanMs=static_cast<double>(se)*toMs/w.jobs;}
    return w;
}
} // namespace nr::model
