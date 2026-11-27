// SPDX-License-Identifier: GPL-3.0-only
// Driver ABI and Feature18 use R2 / tests/nr_compatibility/HardwareSmoke.cpp.
#include "nr_model.h"
#include "nr_model_layout.h"
#include "nr_flow_broker.h"
#include "nr_stabilize.h"
#include "nr_dedup_compare.h"
#include <wrl/wrappers/corewrappers.h>
#include <string>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>
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
// Fixed-size diagnostic slot. A Job retains it from admission to completion; the
// pool never waits, and every value is read only through this slot's own queries.
struct GpuTimingSlot {
    static constexpr UINT queryCount=14,queueStamps=10,markerFirst=10;
    UINT base=0;
    uint64_t frequency=0,markerFrequency=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,4> markerAllocator;
    std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>,4> markerList;
    Microsoft::WRL::ComPtr<ID3D12Fence> markerFence;
    uint64_t markerSerial=0;
    uint32_t stages=0; // 1 dedup, 2 color prep, 4 NGX pass, 8 stabilize, 16 color restore
    uint32_t markers=0; // 1 admitted, 2 bootstrap, 4 refinement, 8 guide conversion
    uint8_t* mapped=nullptr;
    ~GpuTimingSlot() { if(readback && mapped) readback->Unmap(0,nullptr); }
};
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
// Diagnostic split of the per-job "prep" stage (CPU time before the codec encode).
std::atomic<int64_t> prepPre{0},prepGuide{0},prepWorker{0},prepAlloc{0},prepColor{0},prepMid{0},prepCore{0},prepTotal{0};
std::atomic<uint32_t> prepJobs{0};
// Exact-JobKey telemetry. One record per completed job; every aggregate is a mean
// of per-job values from this window, so a maximum from another job/window can never
// be summed into a different job's attribution.
struct TelemetryRecord {
    uint64_t instance=0,generation=0,serial=0,ingress=0;
    const char* kind="none";
    int64_t settleTicks=-1,dedupTicks=-1;
    JobGpuTiming gpu;
    int64_t preTicks=-1,submitTicks=-1,admitTicks=-1,deltaTicks=-1;
};
struct TelemetryAcc { double sum=0; unsigned n=0; };
struct TelemetryAggState {
    unsigned jobs=0;
    TelemetryAcc settle,dedup,dedupGpu,ofaBoot,ofaRefine,guides,prep,ngx,stab,restore,gpu;
    TelemetryAcc preSubmit,submitToDone,admitToDone;
    double identityDeltaMax=-1;
};
struct TelemetryState {
    std::mutex lock;
    std::array<TelemetryRecord,16> ring{};
    unsigned head=0;
    TelemetryAggState agg;
};
TelemetryState telemetryState;
TelemetryRecord telemetry_record(const Job& job) {
    TelemetryRecord record;
    const auto& c=job.chronology;
    record.instance=job.contract.key.frame.instanceId;
    record.generation=job.contract.key.resourceGeneration;
    record.serial=job.contract.key.submissionSerial;
    record.ingress=job.contract.key.frame.ingressSequence;
    record.kind=job.reused?"reuse":job.warmup?"warmup":job.evaluated?"evaluation":"creation";
    record.settleTicks=job.settleWaitQpc;
    record.dedupTicks=job.dedupWaitQpc;
    record.gpu=job.gpuTiming;
    const auto admitted=job.admittedQpc;
    if(admitted>0 && c[5]>=admitted && c[6]>=c[5]) {
        record.preTicks=c[5]-admitted;
        record.submitTicks=c[6]-c[5];
        record.admitTicks=c[6]-admitted;
        record.deltaTicks=record.admitTicks-(record.preTicks+record.submitTicks);
    }
    return record;
}
std::string telemetry_json(const TelemetryRecord& record) {
    const auto ms=[](int64_t value) { return value>=0?qpc_ms(value):-1.0; };
    char buffer[1024];
    std::snprintf(buffer,sizeof buffer,
        "{\"instance\":%llu,\"gen\":%llu,\"serial\":%llu,\"ingress\":%llu,\"kind\":\"%s\","
        "\"settleWaitMs\":%.3f,\"dedupWaitMs\":%.3f,\"dedupGpuMs\":%.3f,\"ofaBootstrapMs\":%.3f,"
        "\"ofaRefineMs\":%.3f,\"guidesShaderMs\":%.3f,\"colorPrepMs\":%.3f,\"ngxPassMs\":%.3f,"
        "\"stabilizeMs\":%.3f,\"colorRestoreMs\":%.3f,\"gpuMs\":%.3f,"
        "\"preSubmitMs\":%.3f,\"submitToDoneMs\":%.3f,\"admitToDoneMs\":%.3f,\"identityDeltaMs\":%.4f}",
        static_cast<unsigned long long>(record.instance),static_cast<unsigned long long>(record.generation),
        static_cast<unsigned long long>(record.serial),static_cast<unsigned long long>(record.ingress),record.kind,
        ms(record.settleTicks),ms(record.dedupTicks),record.gpu.dedupGpuMs,record.gpu.ofaBootstrapMs,
        record.gpu.ofaRefineMs,record.gpu.guidesShaderMs,record.gpu.colorPrepMs,record.gpu.ngxPassMs,
        record.gpu.stabilizeMs,record.gpu.colorRestoreMs,record.gpu.gpuMs,
        ms(record.preTicks),ms(record.submitTicks),ms(record.admitTicks),ms(record.deltaTicks));
    return buffer;
}
JobTelemetry public_telemetry(const TelemetryRecord& record) {
    const auto ms=[](int64_t value) { return value>=0?qpc_ms(value):-1.0; };
    JobTelemetry value;
    value.instance=record.instance;value.generation=record.generation;
    value.serial=record.serial;value.ingress=record.ingress;value.kind=record.kind;
    value.settleWaitMs=ms(record.settleTicks);value.dedupWaitMs=ms(record.dedupTicks);
    value.gpu=record.gpu;
    value.preSubmitMs=ms(record.preTicks);value.submitToDoneMs=ms(record.submitTicks);
    value.admitToDoneMs=ms(record.admitTicks);value.identityDeltaMs=ms(record.deltaTicks);
    return value;
}
void record_telemetry(const Job& job) {
    if(!job.contract.key.submissionSerial || job.chronology[6]<=0) return;
    const auto record=telemetry_record(job);
    std::lock_guard<std::mutex> guard(telemetryState.lock);
    telemetryState.ring[telemetryState.head++&15]=record;
    auto& agg=telemetryState.agg;
    ++agg.jobs;
    const auto add=[](TelemetryAcc& acc,double value) { if(value>=0.0){acc.sum+=value;++acc.n;} };
    const auto ms=[](int64_t value) { return value>=0?qpc_ms(value):-1.0; };
    add(agg.settle,ms(record.settleTicks));add(agg.dedup,ms(record.dedupTicks));
    add(agg.dedupGpu,record.gpu.dedupGpuMs);add(agg.ofaBoot,record.gpu.ofaBootstrapMs);
    add(agg.ofaRefine,record.gpu.ofaRefineMs);add(agg.guides,record.gpu.guidesShaderMs);
    add(agg.prep,record.gpu.colorPrepMs);add(agg.ngx,record.gpu.ngxPassMs);
    add(agg.stab,record.gpu.stabilizeMs);add(agg.restore,record.gpu.colorRestoreMs);
    add(agg.gpu,record.gpu.gpuMs);
    add(agg.preSubmit,ms(record.preTicks));add(agg.submitToDone,ms(record.submitTicks));
    add(agg.admitToDone,ms(record.admitTicks));
    if(record.deltaTicks>=0)
        agg.identityDeltaMax=std::max(agg.identityDeltaMax,std::fabs(qpc_ms(record.deltaTicks)));
}
struct TimingAgg {
    std::atomic<unsigned> jobs{0};
    std::atomic<int64_t> sumSubmit{0},maxSubmit{0},sumEncode{0},maxEncode{0},sumAdmit{0},maxAdmit{0};
    std::atomic<unsigned> admitJobs{0},late{0},stageJobs{0},queueJobs{0},creationJobs{0};
    std::atomic<int64_t> sumCreation{0},maxCreation{0};
    std::atomic<int64_t> sumProd{0},maxProd{0},sumPrep{0},sumRec{0},sumGpu{0},sumQueue{0},maxQueue{0};
};
TimingAgg timingAgg;
struct JobTraceEntry {std::atomic<uint64_t> serial{0};std::atomic<int64_t> queue{0},run{0},total{0};std::atomic<int> late{0};};
JobTraceEntry jobTrace[16];
std::atomic<unsigned> jobTraceHead{0};
struct EligEntry {std::atomic<uint64_t> serial{0};std::atomic<uint32_t> invalid{0},code{0},flags{0};}; // diagnostic: publication eligibility
EligEntry eligTrace[16];
std::atomic<unsigned> eligHead{0};
struct EligAgg {std::atomic<unsigned> n{0},bad{0},badHist{0},badReset{0},orMask{0},noEval{0},noSubmit{0},incomplete{0},qa{0};};
EligAgg eligAgg;
std::atomic<unsigned> dedupSkippedNotReady{0};
// A used asynchronous dedup comparison is the job's own GPU stage even though its
// query heap is not the job timer's. Four exact-serial slots cover completion lag.
struct DedupGpuSample {std::atomic<uint64_t> serial{0};std::atomic<int64_t> micros{-1};};
std::array<DedupGpuSample,4> dedupGpuSamples;
void store_dedup_gpu(uint64_t serial,double ms) {
    auto& slot=dedupGpuSamples[serial&3];
    slot.micros.store(ms>=0.0?static_cast<int64_t>(ms*1000.0):-1,std::memory_order_relaxed);
    slot.serial.store(serial,std::memory_order_release);
}
double take_dedup_gpu(uint64_t serial) {
    auto& slot=dedupGpuSamples[serial&3];
    if(slot.serial.load(std::memory_order_acquire)!=serial)return -1.0;
    const auto micros=slot.micros.load(std::memory_order_relaxed);
    return micros>=0?static_cast<double>(micros)/1000.0:-1.0;
}
std::atomic<uint32_t> appliedPerf{0},appliedGrid{0},appliedHint{0}; // diagnostic: estimator settings actually in use
void raise_max(std::atomic<int64_t>& slot,int64_t value) {
    auto seen=slot.load(std::memory_order_relaxed);
    while(value>seen && !slot.compare_exchange_weak(seen,value,std::memory_order_relaxed)) {}
}
void record_timing(const Job& job) { // diagnostic only: evaluation jobs carry an encode stamp
    record_telemetry(job);
    const auto& c=job.chronology;
    const auto& q=job.qaTiming;
    if(c[1]<=0 && c[0]>0 && c[5]>0 && c[6]>=c[0]) {
        timingAgg.creationJobs.fetch_add(1,std::memory_order_relaxed);const auto cd=c[6]-c[0];
        timingAgg.sumCreation.fetch_add(cd,std::memory_order_relaxed);raise_max(timingAgg.maxCreation,cd);return;
    }
    if(job.warmup || c[1]<=0 || c[5]<=0 || c[6]<c[5] || c[5]<c[1]) return;
    const auto submit=c[6]-c[5],encode=c[6]-c[1];
    timingAgg.jobs.fetch_add(1,std::memory_order_relaxed);
    timingAgg.sumSubmit.fetch_add(submit,std::memory_order_relaxed);timingAgg.sumEncode.fetch_add(encode,std::memory_order_relaxed);
    raise_max(timingAgg.maxSubmit,submit);raise_max(timingAgg.maxEncode,encode);
    const auto admit=job.admittedQpc;
    if(admit>0 && c[6]>=admit) {
        const auto total=c[6]-admit;
        timingAgg.admitJobs.fetch_add(1,std::memory_order_relaxed);timingAgg.sumAdmit.fetch_add(total,std::memory_order_relaxed);
        raise_max(timingAgg.maxAdmit,total);
    }
    const auto check=job.deadlineQpc.load(std::memory_order_relaxed);
    const bool late=check>0 && c[6]>check;
    if(late) timingAgg.late.fetch_add(1,std::memory_order_relaxed);
    if(c[0]>0 && q[1]>=c[0] && c[1]>=q[1] && c[5]>=c[1]) {
        timingAgg.stageJobs.fetch_add(1,std::memory_order_relaxed);
        const auto prod=q[1]-c[0];
        timingAgg.sumProd.fetch_add(prod,std::memory_order_relaxed);raise_max(timingAgg.maxProd,prod);
        timingAgg.sumPrep.fetch_add(c[1]-q[1],std::memory_order_relaxed);
        timingAgg.sumRec.fetch_add(c[5]-c[1],std::memory_order_relaxed);
        timingAgg.sumGpu.fetch_add(c[6]-c[5],std::memory_order_relaxed);
        {
            auto& e=jobTrace[jobTraceHead.fetch_add(1,std::memory_order_relaxed)&15];
            e.serial.store(job.contract.key.submissionSerial);
            e.queue.store(job.enqueuedQpc>0&&job.dequeuedQpc>=job.enqueuedQpc?job.dequeuedQpc-job.enqueuedQpc:0);
            e.run.store(c[6]-c[0]);
            e.total.store(admit>0&&c[6]>=admit?c[6]-admit:0);e.late.store(late?1:0);
        }
        if(job.enqueuedQpc>0 && job.dequeuedQpc>=job.enqueuedQpc) {
            const auto qd=job.dequeuedQpc-job.enqueuedQpc;
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
ComPtr<ID3D12Resource> texture(ID3D12Device* device,Extent size,D3D12_RESOURCE_STATES initial) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=size.width; desc.Height=size.height; desc.DepthOrArraySize=desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        initial,nullptr,IID_PPV_ARGS(&result)),"model color allocation");
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
void read_gpu_timing(Job&); // frees the job's slot after this job's own completion
// Diagnostic-only timestamp owner: one query heap shared by four readback slots,
// plus a dedicated marker queue that timestamps OFA fence milestones. The hot path
// only records EndQuery/ResolveQueryData and enqueues; it never waits for a query.
class GpuTimeline {
public:
    static constexpr UINT slotCount=4;
    // Diagnostic teardown only (never record_timing): bound any marker-queue lag so
    // a readback is not released while its last resolve may still be in flight.
    ~GpuTimeline() {
        for(auto& slot:slots) {
            if(!slot || !slot->markerFence || !slot->markerSerial) continue;
            const auto completed=slot->markerFence->GetCompletedValue();
            if(completed==UINT64_MAX || completed>=slot->markerSerial) continue;
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!event) continue;
            if(SUCCEEDED(slot->markerFence->SetEventOnCompletion(slot->markerSerial,event)))
                (void)WaitForSingleObject(event,20);
            CloseHandle(event);
        }
    }
    void ensure(ID3D12Device* device,ID3D12CommandQueue* queue) {
        if(initialized) return;
        initialized=true;
        if(!trace_job_timeline()) return;
        char injection[8]{};
        if(GetEnvironmentVariableA("OBS_NR_TEST_GPU_TIMER_FAIL",injection,sizeof injection) && injection[0]) return;
        if(!device || !queue) return;
        UINT64 frequency=0;
        if(FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency) return;
        D3D12_QUERY_HEAP_DESC desc{}; desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        desc.Count=slotCount*GpuTimingSlot::queryCount;
        if(FAILED(device->CreateQueryHeap(&desc,IID_PPV_ARGS(&queries)))) return;
        D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&markers)))) { queries.Reset(); return; }
        UINT64 markerFrequency=0;
        if(FAILED(markers->GetTimestampFrequency(&markerFrequency)) || !markerFrequency) {
            queries.Reset(); markers.Reset(); return;
        }
        for(UINT i=0;i<slotCount;++i) {
            auto slot=std::make_shared<GpuTimingSlot>();
            slot->base=i*GpuTimingSlot::queryCount;
            slot->frequency=frequency; slot->markerFrequency=markerFrequency;
            D3D12_HEAP_PROPERTIES allocation{}; allocation.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=256;
            buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1;
            buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if(FAILED(device->CreateCommittedResource(&allocation,D3D12_HEAP_FLAG_NONE,&buffer,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot->readback)))) { fail(); return; }
            if(FAILED(slot->readback->Map(0,nullptr,reinterpret_cast<void**>(&slot->mapped))) || !slot->mapped) {
                fail(); return;
            }
            std::memset(slot->mapped,0,256);
            if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&slot->markerFence)))) { fail(); return; }
            for(UINT k=0;k<4;++k) {
                if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&slot->markerAllocator[k])))) { fail(); return; }
                if(FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
                    slot->markerAllocator[k].Get(),nullptr,IID_PPV_ARGS(&slot->markerList[k])))) { fail(); return; }
                if(FAILED(slot->markerList[k]->Close())) { fail(); return; }
            }
            slots[i]=std::move(slot);
        }
        enabled=true;
    }
    bool ready() const { return enabled; }
    ID3D12QueryHeap* heap() const { return enabled?queries.Get():nullptr; }
    // Called on the worker immediately after a successful guides submit. Cross-queue
    // waits timestamp the actual OFA fence milestones without touching nr_guides.
    void observe_guides(GpuTimingSlot& slot,const FlowResult& flow) {
        if(!enabled || !markers) return;
        const std::array<NV_OF_FENCE_POINT,4> points{flow.inputAdmitted,flow.bootstrapComplete,
            flow.refinementComplete,flow.complete};
        for(UINT i=0;i<points.size();++i) {
            const auto& point=points[i];
            if(!point.fence || !point.value) continue;
            auto& allocator=slot.markerAllocator[i];
            auto& list=slot.markerList[i];
            if(FAILED(allocator->Reset()) ||
               FAILED(list->Reset(allocator.Get(),nullptr))) { enabled=false; return; }
            list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,
                slot.base+GpuTimingSlot::markerFirst+i);
            list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,
                slot.base+GpuTimingSlot::markerFirst+i,1,slot.readback.Get(),128+i*8);
            if(FAILED(list->Close())) { enabled=false; return; }
            if(FAILED(markers->Wait(point.fence,point.value))) { enabled=false; return; }
            ID3D12CommandList* lists[]{list.Get()};
            markers->ExecuteCommandLists(1,lists);
            slot.markers|=1u<<i;
        }
        if(FAILED(markers->Signal(slot.markerFence.Get(),++slot.markerSerial))) { enabled=false; return; }
    }
    std::shared_ptr<GpuTimingSlot> acquire() {
        if(!enabled) return {};
        for(auto& slot:slots) {
            if(!slot || slot.use_count()!=1 || !slot->markerFence || !slot->mapped) continue;
            const auto completed=slot->markerFence->GetCompletedValue();
            if(completed==UINT64_MAX || (slot->markerSerial && completed<slot->markerSerial)) continue;
            slot->stages=slot->markers=0;
            std::memset(slot->mapped,0,256); // never attribute a previous job's stamp
            return slot;
        }
        return {};
    }
private:
    void fail() {
        enabled=false; queries.Reset(); markers.Reset();
        for(auto& slot:slots) slot.reset();
    }
    bool initialized=false,enabled=false;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> markers;
    std::array<std::shared_ptr<GpuTimingSlot>,slotCount> slots;
};
struct Driver;
// Worker-only; neither the allocator nor an immutable analysis lease can retire
// before the compare's own exact fence, even if the readback budget expires.
struct DedupCompare {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Resource> zero,count,readback,stamps;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::Wrappers::Event event;
    uint64_t serial=0,frequency=0;
    uint32_t width=0,height=0;
    bool pending=false;
    std::array<std::shared_ptr<const Frame>,2> leases;
    explicit DedupCompare(ID3D12Device* device)
        :descriptors(heap(device,2)),zero(buffer(device,D3D12_HEAP_TYPE_UPLOAD)),
         count(buffer(device,D3D12_HEAP_TYPE_DEFAULT)),readback(buffer(device,D3D12_HEAP_TYPE_READBACK)),
         event(CreateEventW(nullptr,FALSE,FALSE,nullptr)) {
        require(event.IsValid(),"NR compare event");
        checked(device->CreateRootSignature(0,nr_dedup_compare,sizeof nr_dedup_compare,IID_PPV_ARGS(&root)),"NR compare root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature=root.Get();desc.CS={nr_dedup_compare,sizeof nr_dedup_compare};
        checked(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline)),"NR compare pipeline");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"NR compare allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"NR compare list");
        checked(commands->Close(),"NR compare initial close");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"NR compare fence");
        D3D12_QUERY_HEAP_DESC heapDesc{};heapDesc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;heapDesc.Count=2;
        checked(device->CreateQueryHeap(&heapDesc,IID_PPV_ARGS(&queries)),"NR compare query heap");
        D3D12_HEAP_PROPERTIES read{};read.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC stampDesc{};stampDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        stampDesc.Width=16;stampDesc.Height=1;stampDesc.DepthOrArraySize=stampDesc.MipLevels=1;
        stampDesc.SampleDesc.Count=1;stampDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        checked(device->CreateCommittedResource(&read,D3D12_HEAP_FLAG_NONE,&stampDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&stamps)),"NR compare stamp readback");
        void* mapped=nullptr;const D3D12_RANGE empty{};
        checked(zero->Map(0,&empty,&mapped),"NR compare zero");std::memset(mapped,0,4);zero->Unmap(0,nullptr);
    }
    bool retired() const {
        if(!pending)return true;
        const auto done=fence->GetCompletedValue();
        return done!=UINT64_MAX && done>=serial;
    }
    // Submit exactly one comparison and return without touching its readback. The
    // result is consumed by a later poll only when its own fence has signalled; a
    // stale, unconsumed result is dropped before the next submission.
    bool begin(const interop::Context& context,std::array<std::shared_ptr<const Frame>,2> images,
        NV_OF_FENCE_POINT producer) {
        if(!retired())return false;
        if(pending) {pending=false;leases={};}
        leases=std::move(images);
        if(!leases[0] || !leases[1]) {leases={};return false;}
        auto* device=context.device();
        if(!frequency) checked(context.queue()->GetTimestampFrequency(&frequency),"NR compare timestamp frequency");
        checked(allocator->Reset(),"NR compare allocator reset");
        checked(commands->Reset(allocator.Get(),pipeline.Get()),"NR compare reset");
        if(producer.fence && producer.value)
            checked(context.queue()->Wait(producer.fence,producer.value),"NR compare producer dependency");
        commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        auto cpu=descriptors->GetCPUDescriptorHandleForHeapStart();
        for(const auto& frame:leases) {
            auto* image=frame->analysis.Get();
            device->CreateShaderResourceView(image,nullptr,cpu);
            cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            barrier(commands.Get(),image,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        commands->CopyBufferRegion(count.Get(),0,zero.Get(),0,4);
        barrier(commands.Get(),count.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetComputeRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{descriptors.Get()};commands->SetDescriptorHeaps(1,heaps);
        commands->SetComputeRootDescriptorTable(0,descriptors->GetGPUDescriptorHandleForHeapStart());
        commands->SetComputeRootUnorderedAccessView(1,count->GetGPUVirtualAddress());
        const auto d=leases[0]->analysis->GetDesc();
        width=static_cast<uint32_t>(d.Width);height=static_cast<uint32_t>(d.Height);
        commands->Dispatch((width+15)/16,(height+15)/16,1);
        barrier(commands.Get(),count.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(readback.Get(),0,count.Get(),0,4);
        barrier(commands.Get(),count.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        for(const auto& frame:leases)
            barrier(commands.Get(),frame->analysis.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        commands->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        commands->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,stamps.Get(),0);
        checked(commands->Close(),"NR compare close");
        ID3D12CommandList* lists[]{commands.Get()};
        context.queue()->ExecuteCommandLists(1,lists);
        // The list is submitted: from here a failure must retain its inputs for the
        // fence owner, never free them under an executing command list.
        pending=true;
        checked(context.queue()->Signal(fence.Get(),++serial),"NR compare signal");
        if(qa_sync()) {
            ResetEvent(event.Get());
            checked(fence->SetEventOnCompletion(serial,event.Get()),"NR compare QA subscription");
        }
        return true;
    }
    // nullopt means the readback has not signalled: the caller evaluates NR in full
    // instead of waiting. The compare retires later and is discarded, never reused.
    std::optional<bool> poll(uint64_t jobSerial) {
        if(!pending || unsignalled())return std::nullopt;
        if(qa_sync() && WaitForSingleObject(event.Get(),16)!=WAIT_OBJECT_0)return std::nullopt;
        if(!retired())return std::nullopt;
        pending=false;
        void* mapped=nullptr;const D3D12_RANGE range{0,16},empty{};
        checked(stamps->Map(0,&range,&mapped),"NR compare stamp readback");
        std::array<uint64_t,2> values{};
        std::memcpy(values.data(),mapped,16);stamps->Unmap(0,&empty);
        const double ms=values[1]>values[0] && frequency?
            1000.0*static_cast<double>(values[1]-values[0])/static_cast<double>(frequency):-1.0;
        store_dedup_gpu(jobSerial,ms);
        leases={};
        void* result=nullptr;const D3D12_RANGE resultRange{0,4};
        checked(readback->Map(0,&resultRange,&result),"NR compare readback");
        uint32_t over=0;std::memcpy(&over,result,4);readback->Unmap(0,&empty);
        return over<=std::max<uint64_t>(1,uint64_t{width}*height*3*5/1'000'000);
    }
    // QA-only injected readback silence. The real submission and fence stay intact;
    // only the consumer pretends the signal never arrived.
    static bool unsignalled() {
        char value[2]{};
        size_t size=0;
        return getenv_s(&size,value,sizeof value,"OBS_NR_TEST_DEDUP_UNSIGNALLED")==0 && size==sizeof(value) && value[0]=='1';
    }
    // QA-only deterministic readback for the threshold/invariant fixtures. The
    // production path above never waits; this mirrors the retired synchronous check.
    static bool qa_sync() {
        char value[2]{};
        size_t size=0;
        return getenv_s(&size,value,sizeof value,"OBS_NR_TEST_DEDUP_SYNC")==0 && size==sizeof(value) && value[0]=='1';
    }
};
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
bool Job::ready_for_arrival(std::stop_token stop,uint64_t frameIntervalNs) const {
    if(stop.stop_requested())return false;
    if(ready.load(std::memory_order_acquire))return true;
    if(fault!=Fault::None)return false;
    const auto ms=frameIntervalNs?std::min<uint64_t>(16,frameIntervalNs/1'000'000):16;
    if(!ms)return false;
    Microsoft::WRL::Wrappers::Event wake(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    require(wake.IsValid(),"model settle cancellation event");
    const std::stop_callback cancel(stop,[&wake]{SetEvent(wake.Get());});
    const HANDLE events[]{wake.Get(),observed};
    const auto result=WaitForMultipleObjects(2,events,FALSE,static_cast<DWORD>(ms));
    return (result==WAIT_OBJECT_0+1 || result==WAIT_TIMEOUT) && !stop.stop_requested() && ready.load(std::memory_order_acquire);
}
// Per-job model scratch (five FP16 colors, status buffers, three descriptor heaps) took ~6 ms of CPU per job at
// 1440p, enough on its own to push 60 fps jobs past their one-tick deadline. A slab returns here only when its
// job's Resources dies (where it used to be released) and only if its own command list executed, so every
// resource ends in COMMON; take() also requires that job's completion fence. Every pass writes each texel and
// descriptor before reading it, so a reused slab never shows stale contents.
struct ColorSlab {
    std::array<ComPtr<ID3D12Resource>,8> colors;
    ComPtr<ID3D12Resource> status,statusRead,zero;
    ComPtr<ID3D12DescriptorHeap> codecHeap,prepareHeap,restoreHeap,stabilizeHeap;
    ComPtr<ID3D12Fence> complete;
};
struct ColorSlabPool {
    const ColorLayout layout;
    std::mutex lock;
    std::vector<ColorSlab> free;
    explicit ColorSlabPool(ColorLayout value):layout(value) {}
    std::optional<ColorSlab> take() {
        std::lock_guard<std::mutex> guard(lock);
        for(auto it=free.begin();it!=free.end();++it) {
            const auto done=it->complete->GetCompletedValue();
            if(done==UINT64_MAX || done<1) continue;
            ColorSlab slab=std::move(*it); free.erase(it); return slab;
        }
        return std::nullopt;
    }
    void give(ColorSlab slab) {
        std::lock_guard<std::mutex> guard(lock);
        if(free.size()<2) free.push_back(std::move(slab));
    }
};
struct Job::Resources {
    const std::shared_ptr<interop::Bridge> bridge;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> creation;
    std::array<ComPtr<ID3D12Resource>,8> colors;
    std::array<ComPtr<ID3D12Resource>,4> guideObservations;
    ComPtr<ID3D12Resource> status, statusRead, zero;
    ComPtr<ID3D12DescriptorHeap> codecHeap,prepareHeap,restoreHeap,stabilizeHeap;
    std::shared_ptr<D3D12ColorPass> color;
    std::unique_ptr<Core> core;
    ComPtr<ID3D12Fence> complete;
    std::shared_ptr<runtime::Submission> submission=std::make_shared<runtime::Submission>();
    HANDLE wait=nullptr;
    bool statusRecorded=false;
    std::shared_ptr<ColorSlabPool> pool;
    bool slabExecuted=false; // the worker list holding the end-of-job COMMON barriers was submitted
    explicit Resources(std::shared_ptr<interop::Bridge> transport):bridge(std::move(transport)) {}
    ~Resources() {
        if(!pool || !slabExecuted || !colors[0] || !complete) return;
        try { pool->give({colors,status,statusRead,zero,codecHeap,prepareHeap,restoreHeap,stabilizeHeap,complete}); }
        catch(...) {} // Not pooling only means the next job allocates afresh.
    }
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
ID3D12Resource* Job::answer_resource() const {
    return resources && completedPasses?resources->colors[completedPasses%2?3:6].Get():nullptr;
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
// Reads this job's own slot after its completion. Nothing waits: a slot whose
// marker fence has not retired leaves only those fields unavailable (-1.0).
void read_gpu_timing(Job& job) {
    auto slot=job.gpuSlot;
    if(!slot) return;
    const auto* values=slot->mapped?reinterpret_cast<const uint64_t*>(slot->mapped):nullptr;
    const auto queueValue=job.resources && job.resources->complete?job.resources->complete->GetCompletedValue():0;
    const bool queueComplete=values && job.resources && job.resources->complete &&
        queueValue>=1 && queueValue!=UINT64_MAX;
    const auto markerValue=slot->markerFence?slot->markerFence->GetCompletedValue():0;
    const bool markersComplete=values && slot->markerFence && slot->markerSerial &&
        markerValue>=slot->markerSerial && markerValue!=UINT64_MAX;
    const auto elapsed=[&](UINT first,uint64_t frequency) {
        if(!values || !frequency) return -1.0;
        const auto begin=values[first],end=values[first+1];
        if(!begin || !end || end<=begin) return -1.0;
        return 1000.0*static_cast<double>(end-begin)/static_cast<double>(frequency);
    };
    const auto interval=[&](uint64_t begin,uint64_t end) {
        if(!begin || !end || end<=begin || !slot->markerFrequency) return -1.0;
        return 1000.0*static_cast<double>(end-begin)/static_cast<double>(slot->markerFrequency);
    };
    auto& gpu=job.gpuTiming;
    if(queueComplete) {
        if(slot->stages&1) gpu.dedupGpuMs=elapsed(0,slot->frequency);
        if(slot->stages&2) gpu.colorPrepMs=elapsed(2,slot->frequency);
        if(slot->stages&4) gpu.ngxPassMs=elapsed(4,slot->frequency);
        if(slot->stages&8) gpu.stabilizeMs=elapsed(6,slot->frequency);
        if(slot->stages&16) gpu.colorRestoreMs=elapsed(8,slot->frequency);
    }
    if(gpu.dedupGpuMs<0.0) gpu.dedupGpuMs=take_dedup_gpu(job.contract.key.submissionSerial);
    // Marker resolves land at byte offset 128 of this slot's readback (uint64 index 16).
    if(markersComplete) {
        if((slot->markers&3)==3) gpu.ofaBootstrapMs=interval(values[16],values[17]);
        if((slot->markers&6)==6) gpu.ofaRefineMs=interval(values[17],values[18]);
        if((slot->markers&12)==12) gpu.guidesShaderMs=interval(values[18],values[19]);
    }
    double sum=0; bool measured=false;
    for(const double value:{gpu.dedupGpuMs,gpu.ofaBootstrapMs,gpu.ofaRefineMs,gpu.guidesShaderMs,
        gpu.colorPrepMs,gpu.ngxPassMs,gpu.stabilizeMs,gpu.colorRestoreMs})
        if(value>=0.0){sum+=value;measured=true;}
    gpu.gpuMs=measured?sum:-1.0;
    job.gpuSlot.reset(); // returns the slot to the pool; the readback is not touched again
}
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
        ((job.evaluated && job.composed) || job.reused) && job.submitted && !job.invalid &&
        r.submission->state()==runtime::Completion::Complete;
    { // diagnostic: why a completed job is or is not publishable
        const bool hist=job.flow.has_value() && !job.flow->reset;
        const bool complete=r.submission->state()==runtime::Completion::Complete;
        const bool qaOk=qa_publication_allowed(job.qaFailure.load(std::memory_order_acquire));
        auto& e=eligTrace[eligHead.fetch_add(1,std::memory_order_relaxed)&15];
        e.serial.store(job.contract.key.submissionSerial);e.invalid.store(job.invalid);e.code.store(job.code);
        e.flags.store((hist?1u:0u)|(job.eligible?2u:0u)|(job.evaluated?4u:0u)|(job.submitted?8u:0u)|(complete?16u:0u)|(qaOk?32u:0u)|(job.flow.has_value()?64u:0u));
        eligAgg.n.fetch_add(1);
        if(!job.eligible){eligAgg.bad.fetch_add(1);(hist?eligAgg.badHist:eligAgg.badReset).fetch_add(1);eligAgg.orMask.fetch_or(job.invalid);
            if(!job.evaluated)eligAgg.noEval.fetch_add(1);if(!job.submitted)eligAgg.noSubmit.fetch_add(1);
            if(!complete)eligAgg.incomplete.fetch_add(1);if(!qaOk)eligAgg.qa.fetch_add(1);}
    }
    job.chronology[7]=stamp();
    read_gpu_timing(job);
    record_timing(job);
    job.ready.store(true,std::memory_order_release);
    observe_completed(job);
    SetEvent(job.observed);
}
void CALLBACK completed(void* opaque,BOOLEAN) {
    std::unique_ptr<std::shared_ptr<Job>> retained(static_cast<std::shared_ptr<Job>*>(opaque));
    auto& job=**retained;
    if(job.timeline)job.timeline->callback=stamp();
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
    const std::array<runtime::Pass*,2> passes;
    const std::weak_ptr<Job> current;
    Evaluator(std::array<runtime::Pass*,2> value,const std::shared_ptr<Job>& job):passes(value),current(job) {}
    EvaluationResult evaluate(const ModelInput& in) override {
        auto job=current.lock(); require(job!=nullptr,"live model evaluation lease");
        if(job->fault==Fault::Evaluation || (job->fault==Fault::LaterEvaluation && in.pass==1))
            return {EvaluationState::Error,0xe018};
        auto* commands=static_cast<ID3D12GraphicsCommandList*>(in.input.commands);
        auto* color=static_cast<ID3D12Resource*>(in.color.native);
        if(in.pass)barrier(commands,color,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const runtime::Inputs inputs{static_cast<ID3D12Resource*>(in.color.native),
            static_cast<ID3D12Resource*>(in.depth.native),static_cast<ID3D12Resource*>(in.motion.native),
            static_cast<ID3D12Resource*>(in.output.native),in.depth.active,in.motion.active,false,
            in.input.history.reset,in.input.mvScaleX,in.input.mvScaleY,true};
        if(job->timeline)job->timeline->evaluationBegin[in.pass]=stamp();
        const auto result=passes[in.pass]->run(in.input.job,inputs,recording(job,commands));
        if(job->timeline)job->timeline->evaluationEnd[in.pass]=stamp();
        if(in.pass)barrier(commands,color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        job->chronology[2]=stamp();
        return {result==NVSDK_NGX_Result_Success?EvaluationState::Evaluated:EvaluationState::Error,
            static_cast<uint32_t>(result)};
    }
};
struct TracedCodec final : Codec {
    std::shared_ptr<D3D12Codec> codec;
    Job& job;
    TracedCodec(std::shared_ptr<D3D12Codec> shared,Job& value):codec(std::move(shared)),job(value) {}
    bool encode(const CoreInput& input,const CodecConstants& constants) override {
        const bool result=codec->encode(input,constants); job.chronology[1]=stamp(); return result;
    }
    bool downsample(const CoreInput& input,const CoreTexture& from,const CoreTexture& to) override {
        return codec->downsample(input,from,to);
    }
    bool clamp(const CoreInput& input,const CoreTexture& from,const CoreTexture& to,uint32_t slot) override {
        if(job.fault==Fault::Clamp && slot==0)return false; // Refuse before recording any descriptors/barriers.
        return codec->clamp(input,from,to,slot);
    }
    bool resolve(const CoreInput& input,const CodecConstants& constants,const CoreTexture& source,const CoreTexture& answer) override {
        const bool result=codec->resolve(input,constants,source,answer); job.chronology[3]=stamp(); return result;
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
    struct PreparedGuides {
        std::unique_ptr<D3D12Guides> guides;
        int64_t begin=0;
        FlowInitTiming timing;
    };
    std::atomic<std::shared_ptr<const lifecycle::VendorTrace>> traceView;
    const std::shared_ptr<interop::Context> context;
    const Config config;
    const uint64_t instance;
    std::thread::id worker;
    std::shared_ptr<SdkSession> session; // Exactly one explicit owner lease.
    std::shared_ptr<Driver> driver;
    std::shared_ptr<runtime::Module> module;
    std::array<std::unique_ptr<runtime::Pass>,2> passes;
    std::atomic<std::shared_ptr<const PrewarmRequest>> prewarmRequest;
    std::shared_ptr<const PrewarmRequest> prewarmStarted,prewarmAttempted,prewarmQueued;
    std::shared_ptr<std::atomic_bool> prewarmRevoked;
    std::array<std::unique_ptr<runtime::Pass>,2> prewarmed;
    bool prewarmAborted=false,prewarmReady=false;
    bool realWork=false;
    int64_t prewarmSince=0;
    PassChain chain;
    std::atomic<uint64_t> passStatus{pack_pass_status({})};
    std::unique_ptr<D3D12Guides> guides;
    // Shared MV ring owned with the guides: same device/queue, same analysis
    // extent, recreated when either changes. Null means export is unavailable
    // and NR still runs; a failed export never blocks or fails admission.
    std::shared_ptr<FlowBroker> flowBroker;
    std::string flowBrokerDiagnostic, flowBrokerReported;
    std::future<PreparedGuides> guidePreparation;
    std::optional<Settings> settings;
    std::optional<Extent> extent;
    std::optional<JobKey> previousKey;
    std::shared_ptr<const Frame> previousFrame;
    // Last evaluated frame, the anchor of the duplicate comparison. A reused frame
    // advances guide/flow history but never this anchor, so repeated small changes
    // cannot accumulate into an unbounded reuse chain.
    std::shared_ptr<const Frame> dedupAnchor;
    std::unique_ptr<DedupCompare> dedup;
    GpuTimeline timer; // optional diagnostic timestamp owner; never blocks
    ComPtr<ID3D12Resource> previousNrColor;
    std::shared_ptr<Job> pending;
    std::shared_ptr<ColorSlabPool> pool;
    // Immutable device-bound pipelines are shared; each job retains its own lease
    // until its command list and descriptors are retired.
    std::shared_ptr<D3D12ColorPass> color;
    std::shared_ptr<D3D12Codec> codec;
    std::unique_ptr<D3D12StabilizePass> stabilizer;
    std::optional<Extent> stabilizeExtent;
    std::array<ComPtr<ID3D12Resource>,2> stabilizeOrig,stabilizeTemporal,stabilizeHold;
    bool stabilizeValid=false;
    uint32_t stabilizeIndex=0;
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
    void diag_fail(uint32_t code,Job& job) {
        diagFail.fetch_add(1);diagLastCode.store(code);const auto n=diagFirstN.load();
        if(n<diagFirst.size() || job.timeline) {
            const auto state=diagSettle.load();
            const auto guideState=guides?guides->state_bits():0u;
            const bool ready=pending && pending->ready.load();
            const bool callback=pending && pending->callbackArrived.load();
            const bool hasFence=pending && pending->resources && pending->resources->complete;
            const auto fence=hasFence?pending->resources->complete->GetCompletedValue():0;
            if(job.timeline) {
                auto& t=*job.timeline;
                t.settle=state;t.guideState=guideState;t.pendingGuide=pendingGuide.has_value();
                t.pendingSerial=pending?pending->contract.key.submissionSerial:0;
                t.pendingReady=ready;t.pendingCallback=callback;t.pendingHasFence=hasFence;t.pendingFence=fence;
            }
            if(n<diagFirst.size()) {
                uint64_t v=code&0xffff;v|=uint64_t(state&3)<<16;v|=uint64_t(guideState&7)<<18;
                if(pending)v|=1ull<<21;if(ready)v|=1ull<<22;if(callback)v|=1ull<<23;
                v|=uint64_t(std::min<uint64_t>(fence,3))<<24;if(pendingGuide)v|=1ull<<26;
                diagFirst[n].store(v);diagFirstN.store(n+1);
            }
        }
        diagSettle.store(0);
    }
    Impl(std::shared_ptr<interop::Context> value,Config input,uint64_t id)
        :context(std::move(value)),config(std::move(input)),instance(id) {
        require(context && id && !config.codec.empty() && !config.color.empty() && !config.guides.empty(),
            "private context and actual compiled model shaders required");
    }
    uint32_t open_session(Fault,const std::shared_ptr<std::atomic_bool>& speculative={});
    void prepare_inactive();
    bool discard_prewarm();
    bool adopt_prewarm(const JobContract&);
    bool settle(std::optional<std::stop_token> arrival={}) {
        if(!pending) return true;
        if(!(arrival?pending->ready_for_arrival(*arrival,config.frameIntervalNs):pending->ready.load(std::memory_order_acquire)))
            {diagSettle.store(1);return false;}
        if(pendingGuide && !guides->collect(*pendingGuide,{pending->resources->complete.Get(),1})) {diagSettle.store(2);return false;}
        const auto completion=pending->resources->submission->state();
        const bool historyRecorded=pending->creationPass || pending->completedPasses || pending->failedPass;
        if(!pending->reused && !chain.settle({pending->completedPasses,pending->failedPass,
            pending->passError?pending->passError:pending->code,
            !historyRecorded || completion==runtime::Completion::Complete || completion==runtime::Completion::Discarded,
            pending->eligible})) {diagSettle.store(3);return false;}
        if(pending->failedPass) {reset=true;lastResetSetter="pass-failed";}
        if(!pending->eligible){previousNrColor.Reset();dedupAnchor.reset();}
        passStatus.store(pack_pass_status(chain.status()),std::memory_order_release);
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
            if(job->timeline)job->timeline->signalBegin=stamp();
            const auto actual=context->queue()->Signal(job->resources->complete.Get(),1);
            if(job->timeline)job->timeline->signalEnd=stamp();
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
uint32_t Processor::Impl::open_session(Fault fault,const std::shared_ptr<std::atomic_bool>& speculative) {
    if(session)return session->state.load(std::memory_order_acquire)==SessionState::Ready?0u:0xe020u;
    auto* device=context->device();
    const auto binding=runtime::resolve_driver();
    const auto acquisition=sessions().acquire(session_key(device,config,binding),speculative);
    if(!acquisition.session)return 0xe020;
    session=acquisition.session;driver=session->driver;
    traceView.store(std::shared_ptr<const lifecycle::VendorTrace>(session,&session->trace),std::memory_order_release);
    if(acquisition.initialize) {
        try {
            HMODULE existing=nullptr;
            require(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                config.runtime.canonicalPath.c_str(),&existing),"pre-existing foreign NR image");
            driver->open(device,config,binding,session->key.data,session->trace);
            if(fault==Fault::AfterCoreInit) {
                sessions().retain(session);return 0xe019;
            }
            session->module=runtime::Module::open(config.runtime,driver->dispatch,driver,device,
                session->key.data,session->key.route,&session->trace);
            require(session->module!=nullptr,"selected runtime module admission");
            sessions().publish(session);
        }catch(...) {sessions().retain(session);throw;}
    }
    module=session->module;
    return 0;
}
bool Processor::Impl::discard_prewarm() {
    if(!prewarmStarted)return true;
    prewarmAborted=true;
    if(!release_passes(prewarmed))return false; // Signal failure is not completion.
    // Leave any speculative SDK lease for normal teardown or a retry below.
    lifecycle::ModelClose proof;
    if(session && (!close_session_module(sessions(),session,proof) ||
        !close_session_core(sessions(),session,proof)))return false;
    module.reset();driver.reset();session.reset();traceView.store({},std::memory_order_release);
    prewarmStarted.reset();prewarmRevoked.reset();prewarmReady=false;prewarmAborted=false;
    return true;
}
void Processor::Impl::prepare_inactive() {
    const auto offered=prewarmRequest.load(std::memory_order_acquire);
    if(prewarmStarted && (prewarmAborted || prewarmRevoked->load(std::memory_order_acquire) || !offered ||
        offered->key!=prewarmStarted->key)) {
        (void)discard_prewarm();
        return;
    }
    if(realWork || !offered || !offered->inactive || prewarmReady)return;
    if(!prewarmStarted) {
        if(prewarmAttempted && prewarmAttempted->key==offered->key)return;
        // Start only for a stable inactive source. Delay acquisition on a busy SDK.
        if(prewarmQueued!=offered) {
            prewarmQueued=offered;prewarmSince=stamp();
            return;
        }
        if(qpc_ms(stamp()-prewarmSince)<500.0 || !sessions().idle_for_prewarm(config.runtime.canonicalPath))return;
        prewarmRevoked=std::make_shared<std::atomic_bool>(false);
        prewarmStarted=offered;
        const auto code=open_session(Fault::None,prewarmRevoked);
        if(code) {
            prewarmStarted.reset();prewarmRevoked.reset();prewarmSince=stamp();
            return;
        }
        prewarmAttempted=offered;
    }
    if(prewarmRevoked->load(std::memory_order_acquire) ||
        prewarmRequest.load(std::memory_order_acquire)!=offered)return;
    PassChain two;two.reset(2);
    const auto gate=two.gate(prewarmed,epoch+1);
    if(gate.primed()) {prewarmReady=true;return;}
    if(!gate.create)return; // Scheduler service polls the real creation fence, never waits.
    const auto index=*gate.create;
    auto r=std::make_shared<Job::Resources>(nullptr);
    checked(context->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r->allocator)),
        "inactive NR creation allocator");
    checked(context->device()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r->allocator.Get(),nullptr,
        IID_PPV_ARGS(&r->creation)),"inactive NR creation list");
    prewarmed[index]=std::make_unique<runtime::Pass>(module);
    const auto& key=offered->key;
    const auto& s=key.settings;
    const auto& tune=index && s.pass2Tuning?*s.pass2Tuning:s.tuning;
    const ModelTuning tuning{tune.preset,tune.style,tune.intensity,tune.localStructure,
        index && !s.pass2Tuning?0.0f:tune.localTone,tune.skinStructure,tune.autoMask};
    creationReceipt={};
    const CreateScope scope(driver.get(),creationReceipt);
    // A create-only recording owns a list and allocator, never a capture or Bridge slot.
    NVSDK_NGX_Result result;
    try {
        result=prewarmed[index]->prepare(s,tuning,key.work,offered->history,
            {r->creation.Get(),++epoch,r,[submission=r->submission]{return submission->state();}});
    } catch(...) {
        (void)r->submission->discard();throw;
    }
    if(FAILED(r->creation->Close())) {
        (void)r->submission->discard();prewarmAborted=true;return;
    }
    ID3D12CommandList* lists[]{r->creation.Get()};
    context->queue()->ExecuteCommandLists(1,lists);
    const bool submitted=r->submission->submitted(context->queue());
    prewarmAborted=!submitted || result!=NVSDK_NGX_Result_Success;
    creationEpoch=epoch;
}
bool Processor::Impl::adopt_prewarm(const JobContract& job) {
    if(!prewarmStarted || prewarmAborted)return false;
    const auto luid=context->luid();
    const auto& f=job.input.descriptor;
    const PrewarmKey actual{luid.LowPart,luid.HighPart,config.runtime.sha256,job.input.sourceIdentity,job.input.deviceIdentity,
        job.key.resourceGeneration,f,work_extent(f.active,job.input.settings.workScale),{f.active.width,f.active.height},job.input.settings};
    const auto offered=prewarmRequest.load(std::memory_order_acquire);
    PassChain two;two.reset(2);
    if(!offered || !can_adopt_prewarm(*prewarmStarted,*offered,actual,job.key.historyEpoch,
        two.gate(prewarmed,epoch+1).primed()) ||
        !sessions().adopt(session,prewarmRevoked))return false;
    passes=std::move(prewarmed);prewarmStarted.reset();prewarmRevoked.reset();prewarmReady=false;
    return true;
}
Processor::Processor(std::shared_ptr<interop::Context> context,Config config,uint64_t instance)
    :impl_(std::make_unique<Impl>(std::move(context),std::move(config),instance)) {}
Processor::~Processor() {
    if(!impl_->closed.load(std::memory_order_acquire)) {
        OutputDebugStringA("NR model processor retained: worker cleanup not complete");
        (void)impl_.release();
    }
}
bool Processor::closed() const { return impl_->closed.load(std::memory_order_acquire); }
void Processor::request_prewarm(std::shared_ptr<const PrewarmRequest> offer) noexcept {
    impl_->prewarmRequest.store(std::move(offer),std::memory_order_release);
}
bool Processor::request_original_marker(const std::shared_ptr<Job>& job,const JobKey& key) {
    return job && job->fault==Fault::CompletionSignal && key==job->contract.key &&
        key.frame.instanceId==impl_->instance && job->originalMarker.request(key);
}
void Processor::worker_service() noexcept {
    auto& p=*impl_;
    if(p.closed.load(std::memory_order_acquire))return;
    if(p.worker==std::thread::id{} && p.prewarmRequest.load(std::memory_order_acquire))
        p.worker=std::this_thread::get_id();
    if(p.worker==std::this_thread::get_id() && !p.pending) {
        try { p.prepare_inactive(); }
        catch(const std::exception& e) {
            p.prewarmAborted=true;
            if(p.prewarmStarted)p.prewarmAttempted=p.prewarmStarted;
            OutputDebugStringA(e.what());
        }
    }
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
PassStatus Processor::pass_status() const {return unpack_pass_status(impl_->passStatus.load(std::memory_order_acquire));}
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
    job->enqueuedQpc=work.enqueued;job->dequeuedQpc=work.dequeued;
    auto* t=job->timeline?&*job->timeline:nullptr;
    job->chronology[0]=stamp();if(p.config.qaObservations||job->observation)job->qaTiming[0]=stamp();
    bool dedupQueued=false;
    auto fail=[&](uint32_t code) {
        job->code=code; p.reset=true; p.lastResetSetter="presubmit-failure";
        if(job->gpuSlot) job->gpuSlot->stages=0; // the worker list was not submitted
        p.diag_fail(code,*job);
        if(job->fault!=Fault::None)job->qaFailure.store(code,std::memory_order_release);
        require(work.binding.transport->discard_recording(work.binding.ticket),"model presubmit discard");
        if(t)job->chronology[7]=stamp();
        job->ready.store(true,std::memory_order_release); observe_completed(*job);SetEvent(job->observed);
        return pipeline::Processed{work.job,false,{}};
    };
    try {
    p.realWork=true;
    // Submit the duplicate comparison before the settle wait, under the same
    // failure handling as every other worker-side submission. Its exact fence and
    // leases outlive this job if the compare has not retired; a stale result is
    // discarded by begin(), never by blocking the worker.
    if(job->fault==Fault::None && work.job.input.settings.dedupReuse && work.job.input.settings.valid() &&
       !p.reset && !stop.stop_requested() && p.previousFrame && p.previousKey && p.dedupAnchor && p.previousNrColor) {
        const auto& candidate=work.job.input.descriptor;
        const Extent candidateSize{candidate.active.width,candidate.active.height};
        const bool steady=p.settings && *p.settings==work.job.input.settings &&
            p.extent && *p.extent==candidateSize && p.generation==work.job.key.resourceGeneration &&
            p.previousKey->frame.instanceId==work.job.key.frame.instanceId &&
            p.previousKey->frame.contentEpoch==work.job.key.frame.contentEpoch &&
            p.previousKey->historyEpoch==work.job.key.historyEpoch &&
            p.previousKey->resourceGeneration==work.job.key.resourceGeneration &&
            p.previousKey->frame.ingressSequence+1==work.job.key.frame.ingressSequence &&
            p.previousKey->submissionSerial<work.job.key.submissionSerial &&
            work_admitted(work_extent(candidate.active,work.job.input.settings.workScale));
        if(steady) {
            if(!p.dedup)p.dedup=std::make_unique<DedupCompare>(p.context->device());
            dedupQueued=p.dedup->begin(*p.context,{job->frame,p.dedupAnchor},job->frame->producer);
        }
    }
    p.timer.ensure(p.context->device(),p.context->queue());
    const int64_t settleBegin=stamp();
    const bool stopped=stop.stop_requested();
    const bool settled=!stopped && p.settle(stop);
    const int64_t settleEnd=stamp();
    job->settleWaitQpc=settleEnd-settleBegin;
    if(t) { t->stop=stopped;t->settleEnd=settleEnd; }
    if(!settled) return fail(0xe002);
    job->gpuSlot=p.timer.acquire();
    const auto& d=work.job.input.descriptor;
    const Extent size{d.active.width,d.active.height};
    const auto working=work_extent(d.active,work.job.input.settings.workScale);
    if(!work_admitted(working)) {
        p.passStatus.store(pack_pass_status({work.job.input.settings.passes,0,0,work_scale_refused}),std::memory_order_release);
        return fail(work_scale_refused); // No producer wait, NR session, prepare or submission.
    }
    job->epoch=++p.epoch; job->creationEpoch=p.creationEpoch;
    if(job->fault==Fault::Capability) return fail(0xe003); // Owned admission control, no feature/compose.
    const auto producer=job->frame->producer;
    if(t) { t->producerBegin=stamp();t->producerTarget=producer.value; }
    const auto done=producer.fence->GetCompletedValue();
    if(t)t->producerAtWait=done;
    const auto producerCode=producer_return(job->fault,producer.value,done,[&] {
        if(job->fault==Fault::None) {
            checked(p.context->queue()->Wait(producer.fence,producer.value),"model GPU producer dependency");
            return true;
        }
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"producer event");
        const auto result=producer.fence->SetEventOnCompletion(producer.value,event);
        const auto wait=SUCCEEDED(result)?WaitForSingleObject(event,15000):WAIT_FAILED;
        CloseHandle(event);
        return wait==WAIT_OBJECT_0 && producer.fence->GetCompletedValue()!=UINT64_MAX;
    });
    if(t)t->producerEnd=stamp();
    if(producerCode)return fail(producerCode);
    job->qaTiming[1]=t?t->producerEnd:stamp();
    auto* device=p.context->device();
    const auto plan=color_plan(d,job->exposure,
        typed(device,interop::source_format(d.format),false) &&
        typed(device,DXGI_FORMAT_R16G16B16A16_FLOAT,true) &&
        typed(device,interop::canonical_format(d.format),true));
    if(!plan) return fail(0xe006);
    if(p.prewarmStarted) {
        job->prewarmAdopted=job->fault==Fault::None && p.adopt_prewarm(work.job);
        if(!job->prewarmAdopted && !p.discard_prewarm()) {
            // Keep the raw warmup path until a discarded create retires on
            // its own fence. This is not an NR evaluation/worker failure.
            require(work.binding.transport->discard_recording(work.binding.ticket),"pending inactive prepare raw discard");
            job->chronology[7]=stamp();
            job->ready.store(true,std::memory_order_release);observe_completed(*job);SetEvent(job->observed);
            return {work.job,false,{}};
        }
    }
    const bool adapterChanged=p.settings!=work.job.input.settings || p.extent!=size ||
        p.generation!=work.job.key.resourceGeneration;
    if(adapterChanged || !p.settings) {
        if(!job->prewarmAdopted && !release_passes(p.passes))return fail(0xe007);
        p.previousFrame.reset(); p.previousKey.reset(); p.dedupAnchor.reset();
        if(p.guidePreparation.valid()) {
            p.guidePreparation.wait();
            p.guidePreparation={};
        }
        if(adapterChanged && p.guides) {
            require(p.guides->close(),"old generation guides close"); p.guides.reset();
            p.flowBroker.reset();
        }
        p.settings.emplace(work.job.input.settings);p.extent.emplace(size);p.generation=work.job.key.resourceGeneration;
        p.chain.reset(work.job.input.settings.passes);
        p.passStatus.store(pack_pass_status(p.chain.status()),std::memory_order_release);
        p.reset=true; p.lastResetSetter=job->prewarmAdopted?"inactive-prewarm-adopted":"pass-retired";
    } else if(p.passes[0] && !p.passes[0]->ready(job->epoch)) {
        if(!p.passes[0]->release())return fail(0xe007);
        p.passes[0].reset();p.previousFrame.reset();p.previousKey.reset();p.dedupAnchor.reset();
        p.reset=true;p.lastResetSetter="first-pass-retired";
    }
    if(p.chain.status().failedPass && !release_passes(p.passes,p.chain.limit()))return fail(0xe007);
    if(!p.session) {
        if(t)t->sdkBegin=stamp();
        if(p.config.qaObservations||job->observation)job->qaTiming[2]=stamp();
        if(const auto code=p.open_session(job->fault))return fail(code);
        if(t)t->sdkEnd=stamp();
    }
    if(p.session->state.load(std::memory_order_acquire)!=SessionState::Ready)return fail(0xe020);
    job->resources=std::make_shared<Job::Resources>(work.binding.transport);
    auto& r=*job->resources;
    const auto gate=p.chain.gate(p.passes,job->epoch);
    if(gate.create) {
        const auto index=*gate.create;
        job->creationPass=index+1;
        p.passes[index]=std::make_unique<runtime::Pass>(p.module);
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r.allocator)),
            "owned creation allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r.allocator.Get(),nullptr,
            IID_PPV_ARGS(&r.creation)),"owned creation list");
        p.subscribe(job);
        if(job->fault==Fault::Creation || (job->fault==Fault::LaterCreation && index==1)) {
            r.submission->discard(); checked(r.creation->Close(),"failed creation close");
            work.binding.transport->discard_recording(work.binding.ticket);
            job->code=0xe008;job->failedPass=index+1;p.signal(job); return {work.job,false,{}};
        }
        if(job->warmup && index==0 && job->fault==Fault::None &&
           !p.config.qaObservations && !p.config.qaPreparation && !p.guides && !p.guidePreparation.valid()) {
            // NGX has finished opening. Overlap first feature creation with
            // independent OFA preparation. The future transfers exclusive
            // session ownership; the helper never submits frame/queue work.
            p.guidePreparation=std::async(std::launch::async,
                [context=p.context, allocation=d.allocation, shader=p.config.guides, interval=p.config.frameIntervalNs] {
                    Impl::PreparedGuides prepared;
                    prepared.begin=stamp();
                    prepared.guides=std::make_unique<D3D12Guides>(context->device(),context->queue(),
                        allocation,shader,interval,&prepared.timing);
                    return prepared;
                });
        }
        if(t)t->recordBegin=stamp();
        if(p.config.qaObservations||job->observation)job->qaTiming[3]=stamp();
        p.creationReceipt={};
        const CreateScope createScope(p.driver.get(),p.creationReceipt);
        const auto& s=work.job.input.settings;
        const auto& src=(index && s.pass2Tuning)?*s.pass2Tuning:s.tuning;
        const float tone=(index && !s.pass2Tuning)?0.0f:src.localTone;
        const ModelTuning tuning{src.preset,src.style,src.intensity,src.localStructure,tone,src.skinStructure,src.autoMask};
        const auto code=p.passes[index]->prepare(work.job.input.settings,tuning,working,work.job.key.historyEpoch,recording(job,r.creation.Get()));
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
        if(!job->created)job->failedPass=index+1;
        checked(r.creation->Close(),"owned creation close");
        if(t)t->recordEnd=stamp();
        ID3D12CommandList* lists[]{r.creation.Get()}; p.context->queue()->ExecuteCommandLists(1,lists);
        job->chronology[4]=stamp();
        job->submitted=r.submission->submitted(p.context->queue());
        job->chronology[5]=stamp();
        require(work.binding.transport->discard_recording(work.binding.ticket),"creation raw slot discard");
        p.signal(job);
        return {work.job,false,{}};
    }
    if(!gate.primed())return fail(0xe009); // Never expose a smaller NR count while extra creation is still possible.
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
    if(!p.guides) {
        if(t)t->guideInit=stamp();
        if(p.guidePreparation.valid()) {
            auto prepared=p.guidePreparation.get();
            p.guides=std::move(prepared.guides);
            if(t) { t->flowInitBegin=prepared.begin;t->flowInit=prepared.timing; }
        } else {
            if(t)t->flowInitBegin=t->guideInit;
            p.guides=std::make_unique<D3D12Guides>(device,p.context->queue(),d.allocation,p.config.guides,p.config.frameIntervalNs,
                t?&t->flowInit:nullptr);
        }
    }
    if(p.config.qaPreparation && !p.preparation.load(std::memory_order_relaxed))
        p.preparation.store(std::make_shared<const GuidePreparation>(GuidePreparation{
            work.job.key,p.guides->observation_descriptors(),p.guides->grid_size()}),std::memory_order_release);
    if(!p.flowBroker) {
        p.flowBroker=FlowBroker::create({device,p.context->queue(),d.allocation,p.instance},&p.flowBrokerDiagnostic);
        if(!p.flowBroker && p.flowBrokerDiagnostic!=p.flowBrokerReported) {
            p.flowBrokerReported=p.flowBrokerDiagnostic;
            OutputDebugStringA(("NR shared flow ring unavailable: "+p.flowBrokerDiagnostic+"\n").c_str());
        }
    }
    const int64_t tGuide0=stamp();
    if(t)t->guideSubmit=tGuide0;
    const FlowImage current{work.job.key,job->frame->analysis.Get(),producer};
    std::optional<FlowImage> previous;
    if(!reset) previous.emplace(FlowImage{*p.previousKey,p.previousFrame->analysis.Get(),p.previousFrame->producer});
    const auto flow=p.guides->submit({current,previous?&*previous:nullptr,d.allocation,d.active,reset,work.job.input.settings.stabilize.motionMode});
    if(flow!=FlowProblem::None) return fail(0xe100+static_cast<uint32_t>(flow));
    const auto* guide=p.guides->result(); require(guide && guide->key==work.job.key,"exact native guides");
    if(job->gpuSlot) p.timer.observe_guides(*job->gpuSlot,*guide);
    if(t)t->guideTarget=guide->complete.value;
    job->flow.emplace(*guide);
    job->guideGrid=p.guides->grid_size();
        appliedPerf.store(static_cast<uint32_t>(p.guides->perf_level()));appliedGrid.store(p.guides->grid_size());appliedHint.store(p.guides->hint_grid_size());
    if(previous) job->guideReferenceKey.emplace(previous->key);
    p.pendingGuide.emplace(work.job.key);
    checked(p.context->queue()->Wait(guide->complete.fence,guide->complete.value),"native guide queue dependency");
    const int64_t tGuide1=stamp();
    if(t)t->guideDone=tGuide1;
    // The comparison was submitted before settle; guides now provide the CPU
    // runway to read it without a readback wait. A duplicate still owns its
    // guide submission, so the next evaluation sees contiguous guide history.
    if(work.job.input.settings.dedupReuse && job->fault==Fault::None && !reset &&
       p.dedupAnchor && p.previousNrColor && p.previousKey->submissionSerial<work.job.key.submissionSerial) {
        std::optional<bool> duplicate;
        if(dedupQueued) {
            const int64_t pollQpc=stamp();
            duplicate=p.dedup->poll(work.job.key.submissionSerial);
            job->dedupWaitQpc=stamp()-pollQpc;
            if(!duplicate)dedupSkippedNotReady.fetch_add(1,std::memory_order_relaxed);
        } else {
            job->dedupWaitQpc=0;
            dedupSkippedNotReady.fetch_add(1,std::memory_order_relaxed);
        }
        if(duplicate && *duplicate) {
            const auto view=work.binding.transport->begin_worker(work.binding.ticket);
            require(view.has_value(),"NR reuse Bridge admission");
            job->output=view->output;job->reused=true;job->referenceValid=true;
            auto* list=view->commands;
            p.subscribe(job);
            if(p.flowBroker && previous) {
                const FlowExport flowExport{{work.job.key,work.job.input.canonicalTick},
                    FlowFrameRef{previous->key,work.job.input.precedingCanonicalTick},guide->motion,
                    SharedFlowSlot{d.allocation.width,d.allocation.height,d.active.x,d.active.y,
                        d.active.width,d.active.height,1.0f,1.0f,false,false}};
                (void)p.flowBroker->record_export(list,flowExport);
            }
            barrier(list,p.previousNrColor.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier(list,view->output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(view->output,p.previousNrColor.Get());
            barrier(list,view->output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
            barrier(list,p.previousNrColor.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
            job->submitted=work.binding.transport->submit_worker(work.binding.ticket) && r.submission->submitted(p.context->queue());
            if(p.flowBroker) {
                if(job->submitted) p.flowBroker->commit_export(p.context->queue());
                else p.flowBroker->abandon_export();
            }
            if(job->submitted) {p.previousKey.emplace(work.job.key);p.previousFrame=job->frame;}
            else {p.reset=true;p.lastResetSetter="reuse-submit-failed";}
            p.signal(job);
            return {work.job,job->submitted,job};
        }
    }
    const auto view=work.binding.transport->begin_worker(work.binding.ticket);
    require(view.has_value(),"actual Bridge worker admission");
    job->output=view->output;
    const int64_t tAlloc0=stamp();
    if(t) { t->workerTarget=view->completionValue;t->allocBegin=tAlloc0; }
    const ColorLayout layout{size,working,work.job.input.settings.passes>1};
    if(!p.pool || p.pool->layout!=layout)p.pool=std::make_shared<ColorSlabPool>(layout);
    r.pool=p.pool;
    auto slab=p.pool->take();
    if(slab) {
        r.colors=std::move(slab->colors); r.status=std::move(slab->status); r.statusRead=std::move(slab->statusRead);
        r.zero=std::move(slab->zero); r.codecHeap=std::move(slab->codecHeap);
        r.prepareHeap=std::move(slab->prepareHeap); r.restoreHeap=std::move(slab->restoreHeap);
        r.stabilizeHeap=std::move(slab->stabilizeHeap);
    } else {
        const auto extents=layout.extents();
        const auto initial=layout.initial_states();
        for(size_t i=0;i<r.colors.size();++i) if(extents[i].width)
            r.colors[i]=texture(device,extents[i],initial[i]);
        r.status=buffer(device,D3D12_HEAP_TYPE_DEFAULT); r.statusRead=buffer(device,D3D12_HEAP_TYPE_READBACK);
        r.zero=buffer(device,D3D12_HEAP_TYPE_UPLOAD);
        r.codecHeap=heap(device,25); r.prepareHeap=heap(device,4); r.restoreHeap=heap(device,4);
        if(!p.config.stabilize.empty()) r.stabilizeHeap=heap(device,9);
    }
    const int64_t tAlloc1=stamp();
    if(t) { t->allocEnd=tAlloc1;t->recordBegin=tAlloc1; }
    auto* list=view->commands;
    // The ring copy is ordered after FlowResult::complete and copied here,
    // BEFORE the next collect() retires the private guide texture. A full ring
    // drops this export instead of stalling the worker.
    if(p.flowBroker && !reset && previous) {
        const FlowExport flowExport{{work.job.key,work.job.input.canonicalTick},
            FlowFrameRef{previous->key,work.job.input.precedingCanonicalTick},guide->motion,
            SharedFlowSlot{d.allocation.width,d.allocation.height,d.active.x,d.active.y,
                d.active.width,d.active.height,1.0f,1.0f,false,false}};
        (void)p.flowBroker->record_export(list,flowExport);
    }
    ID3D12QueryHeap* const stampHeap=p.timer.heap();
    const UINT stampBase=job->gpuSlot?job->gpuSlot->base:0;
    const auto stampAt=[&](UINT index) {
        if(stampHeap && job->gpuSlot) list->EndQuery(stampHeap,D3D12_QUERY_TYPE_TIMESTAMP,stampBase+index);
    };
    const auto stampStage=[&](UINT index,uint32_t bit) {
        stampAt(index);
        if(stampHeap && job->gpuSlot) job->gpuSlot->stages|=bit;
    };
    if(slab) { // A returned slab ended its last submitted job in COMMON, including status.
        for(auto& color:r.colors) if(color)
            barrier(list,color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    } else {
        const auto initial=layout.initial_states();
        for(size_t i=0;i<r.colors.size();++i) if(r.colors[i] && initial[i]==D3D12_RESOURCE_STATE_COMMON)
            barrier(list,r.colors[i].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void* mapped=nullptr; const D3D12_RANGE empty{0,0};
    checked(r.zero->Map(0,&empty,&mapped),"owned status zero upload");
    memset(mapped,0,4); r.zero->Unmap(0,nullptr);
    list->CopyBufferRegion(r.status.Get(),0,r.zero.Get(),0,4);
    barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(list,job->frame->original.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const int64_t tColor0=stamp();
    if(!p.color) p.color=std::make_shared<D3D12ColorPass>(device,p.config.color);
    r.color=p.color;
    const int64_t tColor1=stamp();
    barrier(list,r.colors[2].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    stampAt(2);
    require(r.color->record(list,r.prepareHeap.Get(),*plan,job->frame->original.Get(),
        r.colors[2].Get(),r.colors[0].Get(),r.status.Get(),false),"actual canonical color prepare");
    stampStage(3,2);
    barrier(list,r.colors[2].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(list,r.colors[0].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto ct=[&](size_t i) {const auto extent=layout.extents()[i];
        return CoreTexture{r.colors[i].Get(),extent,{0,0,extent.width,extent.height}};};
    const CoreResources resources{ct(0),ct(1),ct(2),ct(3),ct(4),
        {guide->depth,d.allocation,d.active},{guide->motion,d.allocation,d.active},r.codecHeap.Get()};
    // Foreign evaluator consumes explicit readable guides, leaving model output UAV.
    barrier(list,guide->depth,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(list,guide->motion,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const Epochs epochs{work.job.key.frame.instanceId,work.job.key.frame.contentEpoch,
        work.job.key.historyEpoch,work.job.key.resourceGeneration};
    const CoreInput input{work.job,epochs,resources,{work.job.key.historyEpoch,!reset,reset},list,
        plan->whitePoint,float(working.width)/float(size.width),float(working.height)/float(size.height),
        plan->codecColor,plan->codecAlpha,gate.ready,ct(5),ct(6),ct(7)};
    const int64_t tCore0=stamp();
    if(!p.codec) p.codec=std::make_shared<D3D12Codec>(device,p.config.codec);
    r.core=std::make_unique<Core>(p.instance,std::make_unique<TracedCodec>(p.codec,*job),
        std::make_unique<Evaluator>(std::array{p.passes[0].get(),p.passes[1].get()},job));
    const int64_t tCore1=stamp();
    p.subscribe(job);
    stampAt(4);
    const auto result=r.core->record(input);
    stampStage(5,4);
    if(!job->warmup && job->chronology[1]>tCore1) {
        prepPre.fetch_add(tGuide0-job->qaTiming[1],std::memory_order_relaxed);prepGuide.fetch_add(tGuide1-tGuide0,std::memory_order_relaxed);
        prepWorker.fetch_add(tAlloc0-tGuide1,std::memory_order_relaxed);prepAlloc.fetch_add(tAlloc1-tAlloc0,std::memory_order_relaxed);
        prepColor.fetch_add(tColor1-tColor0,std::memory_order_relaxed);prepMid.fetch_add(tCore0-tColor1,std::memory_order_relaxed);
        prepCore.fetch_add(tCore1-tCore0,std::memory_order_relaxed);prepTotal.fetch_add(job->chronology[1]-job->qaTiming[1],std::memory_order_relaxed);
        prepJobs.fetch_add(1,std::memory_order_relaxed);
    }
    job->evaluated=result.evaluated; job->composed=result.composed; job->code=result.error;
    job->completedPasses=result.passes;job->failedPass=result.failedPass;job->passError=result.passError;
    if(result.state!=CoreState::Recorded || job->fault==Fault::Submission) {
        if(p.flowBroker) p.flowBroker->abandon_export(); // the list is never submitted
        if(job->gpuSlot) job->gpuSlot->stages=0; // discarded worker list: no stage resolves execute
        if(t)t->recordEnd=stamp();
        require(r.submission->discard(),"unsubmitted evaluation discard");
        require(work.binding.transport->discard_recording(work.binding.ticket),"failed core Bridge discard");
        if(job->fault==Fault::Submission) job->code=0xe010;
        p.reset=true; p.lastResetSetter="core-record-refused"; p.signal(job); return {work.job,false,{}};
    }
    barrier(list,r.colors[4].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto& stabCfg=work.job.input.settings.stabilize;
    const bool useStabilizer=!p.config.stabilize.empty() && stabCfg.active();
    ID3D12Resource* canonicalOutput=r.colors[4].Get();
    uint32_t stabCur=p.stabilizeIndex, stabPrev=stabCur^1u;
    bool stabDispatched=false;
    if(useStabilizer) {
        if(!p.stabilizer) p.stabilizer=std::make_unique<D3D12StabilizePass>(device,p.config.stabilize);
        if(!r.stabilizeHeap) r.stabilizeHeap=heap(device,9);
        if(!p.stabilizeExtent || *p.stabilizeExtent!=size || !p.stabilizeOrig[0]) {
            for(size_t i=0;i<2;++i) {
                p.stabilizeOrig[i]=texture(device,size,D3D12_RESOURCE_STATE_COMMON);
                p.stabilizeTemporal[i]=texture(device,size,D3D12_RESOURCE_STATE_COMMON);
                p.stabilizeHold[i]=texture(device,size,D3D12_RESOURCE_STATE_COMMON);
            }
            p.stabilizeExtent.emplace(size); p.stabilizeValid=false; p.stabilizeIndex=0;
            stabCur=0; stabPrev=1;
        }
        const bool hasStabHistory=!reset && previous.has_value() && p.stabilizeValid;
        for(auto* res:{p.stabilizeOrig[stabPrev].Get(),p.stabilizeTemporal[stabPrev].Get(),p.stabilizeHold[stabPrev].Get()})
            barrier(list,res,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto* res:{p.stabilizeHold[stabCur].Get(),p.stabilizeTemporal[stabCur].Get(),p.stabilizeOrig[stabCur].Get()})
            barrier(list,res,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        stampAt(6);
        require(p.stabilizer->record(list,r.stabilizeHeap.Get(),size,stabCfg,hasStabHistory,
            r.colors[0].Get(),r.colors[4].Get(),guide->motion,
            p.stabilizeOrig[stabPrev].Get(),p.stabilizeTemporal[stabPrev].Get(),p.stabilizeHold[stabPrev].Get(),
            p.stabilizeHold[stabCur].Get(),p.stabilizeTemporal[stabCur].Get(),p.stabilizeOrig[stabCur].Get()),
            "actual anti-flicker stabilize pass");
        stampStage(7,8);
        barrier(list,p.stabilizeHold[stabCur].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list,p.stabilizeTemporal[stabCur].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        barrier(list,p.stabilizeOrig[stabCur].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        for(auto* res:{p.stabilizeOrig[stabPrev].Get(),p.stabilizeTemporal[stabPrev].Get(),p.stabilizeHold[stabPrev].Get()})
            barrier(list,res,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        canonicalOutput=p.stabilizeHold[stabCur].Get();
        stabDispatched=true;
    } else {
        p.stabilizeValid=false;
    }
    barrier(list,view->output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER order{}; order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; order.UAV.pResource=r.status.Get();
    list->ResourceBarrier(1,&order);
    job->referenceValid=!reset&&previous.has_value();
    stampAt(8);
    require(r.color->record(list,r.restoreHeap.Get(),*plan,job->frame->original.Get(),canonicalOutput,
        view->output,r.status.Get(),true,job->referenceValid),"actual source-relative color restore");
    stampStage(9,16);
    if(stabDispatched) barrier(list,p.stabilizeHold[stabCur].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(r.statusRead.Get(),0,r.status.Get(),0,4); r.statusRecorded=true;
    barrier(list,r.status.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,job->frame->original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,guide->depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    barrier(list,guide->motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    if(work.job.input.settings.dedupReuse && job->fault==Fault::None) {
        // Bridge output slots recycle independently of model history. Keep one
        // private restored-color allocation, never pin a reusable Bridge slot.
        const auto desc=view->output->GetDesc();
        if(!p.previousNrColor || reset) {
            p.previousNrColor.Reset();
            D3D12_HEAP_PROPERTIES allocation{};allocation.Type=D3D12_HEAP_TYPE_DEFAULT;
            auto cache=desc;cache.Flags=D3D12_RESOURCE_FLAG_NONE;
            checked(device->CreateCommittedResource(&allocation,D3D12_HEAP_FLAG_NONE,&cache,D3D12_RESOURCE_STATE_COMMON,
                nullptr,IID_PPV_ARGS(&p.previousNrColor)),"NR duplicate color cache");
        }
        barrier(list,view->output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list,p.previousNrColor.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(p.previousNrColor.Get(),view->output);
        barrier(list,p.previousNrColor.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        barrier(list,view->output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    } else {
        p.previousNrColor.Reset();
        barrier(list,view->output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    }
    const auto states=final_color_states(result.passes);
    for(size_t i=0;i<r.colors.size();++i) if(r.colors[i])
        barrier(list,r.colors[i].Get(),states[i],D3D12_RESOURCE_STATE_COMMON);
    if(auto* o=job->observation) {
        const std::array<ID3D12Resource*,Observation::Count> sources{
            r.colors[0].Get(),r.colors[1].Get(),r.colors[2].Get(),job->answer_resource(),r.colors[4].Get(),
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
    if(stampHeap && job->gpuSlot) {
        // Resolve only this job's own ranges; masks decide which values are attributed.
        list->ResolveQueryData(stampHeap,D3D12_QUERY_TYPE_TIMESTAMP,stampBase+2,4,job->gpuSlot->readback.Get(),16);
        list->ResolveQueryData(stampHeap,D3D12_QUERY_TYPE_TIMESTAMP,stampBase+6,2,job->gpuSlot->readback.Get(),48);
        list->ResolveQueryData(stampHeap,D3D12_QUERY_TYPE_TIMESTAMP,stampBase+8,2,job->gpuSlot->readback.Get(),64);
    }
    if(t)t->recordEnd=stamp();
    job->submitted=work.binding.transport->submit_worker(work.binding.ticket);
    if(!job->submitted && job->gpuSlot) job->gpuSlot->stages=0; // list never executed
    r.slabExecuted=job->submitted;
    job->chronology[4]=stamp();
    if(job->submitted) job->submitted=r.submission->submitted(p.context->queue());
    job->chronology[5]=stamp();
    if(p.flowBroker) {
        if(job->submitted) p.flowBroker->commit_export(p.context->queue());
        else p.flowBroker->abandon_export();
    }
    p.reset=!job->submitted; p.lastResetSetter="submission-result";
    if(stabDispatched && job->submitted) { p.stabilizeValid=true; p.stabilizeIndex=stabCur^1u; }
    else p.stabilizeValid=false;
    p.previousKey.emplace(work.job.key); p.previousFrame=job->frame;
    if(job->submitted)p.dedupAnchor=job->frame;
    p.signal(job);
    p.diagOk.fetch_add(1);
    return {work.job,job->submitted,job};
    } catch(const std::exception& error) {
        job->diagnostic=error.what(); job->code=0xefff; p.reset=true; p.lastResetSetter="process-exception";
        if(p.flowBroker) p.flowBroker->abandon_export();
        if(job->gpuSlot) job->gpuSlot->stages=0;
        p.diagExc.fetch_add(1);if(!p.diagWhatSet.load()){{const char* m=error.what();size_t i=0;for(;m[i]&&i+1<p.diagWhat.size();++i)p.diagWhat[i]=m[i];p.diagWhat[i]=0;}p.diagWhatSet.store(true,std::memory_order_release);}p.diag_fail(0xefff,*job);
        // No recording/resources means no feature submission occurred. All other
        // failures retain their exact resource owners; never manufacture completion.
        if(!job->resources && work.binding.transport->discard_recording(work.binding.ticket)) {
            if(t)job->chronology[7]=stamp();
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
                if(p.dedup && !p.dedup->retired())return false;
                if(!release_passes(p.prewarmed) || !release_passes(p.passes))return false;
                p.dedup.reset();p.previousNrColor.Reset();
                p.previousFrame.reset();p.previousKey.reset();p.dedupAnchor.reset();p.pool.reset();
                p.stabilizeOrig={};p.stabilizeTemporal={};p.stabilizeHold={};p.stabilizer.reset();
                p.stabilizeValid=false;p.stabilizeExtent.reset();
                return true;
            case 2:
                if(p.guidePreparation.valid()) {
                    if(p.guidePreparation.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
                    p.guidePreparation={};
                }
                if(p.guides && !p.guides->close())return false;
                p.guides.reset();
                p.flowBroker.reset(); // unresolved ring ownership is parked, never freed
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
bool trace_job_timeline() noexcept {
    static const bool enabled=[] {
        char value[2]{};
        size_t size=0;
        return getenv_s(&size,value,sizeof(value),"OBS_NR_JOB_TIMELINE")==0 && size==sizeof(value) && value[0]=='1';
    }();
    return enabled;
}
std::string take_job_trace_base() {
    const auto head=jobTraceHead.load(std::memory_order_relaxed);
    std::string out;
    const unsigned n=head<12?head:12;
    for(unsigned k=n;k>0;--k) {
        const auto& e=jobTrace[(head-k)&15];
        char buf[96];
        std::snprintf(buf,sizeof buf,"%s%llu:q%.0f/r%.0f/t%.0f%s",out.empty()?"":",",static_cast<unsigned long long>(e.serial.load()),
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
        std::snprintf(buf,sizeof buf,"%s%llu:i%x/c%x/f%x",k==n?"":",",static_cast<unsigned long long>(e.serial.load()),e.invalid.load(),e.code.load(),e.flags.load());
        out+=buf;
    }
    out+=")";
    if(const auto jobs=prepJobs.exchange(0)) {
        const auto ms=[&](std::atomic<int64_t>& v){return qpc_ms(v.exchange(0))/jobs;};
        const double pre=ms(prepPre),guide=ms(prepGuide),worker=ms(prepWorker),alloc=ms(prepAlloc),color=ms(prepColor),mid=ms(prepMid),core=ms(prepCore),total=ms(prepTotal);
        std::snprintf(buf,sizeof buf," | prep n%u pre%.2f guide%.2f worker%.2f alloc%.2f colorPso%.2f mid%.2f corePso%.2f encode%.2f total%.2f",
            jobs,pre,guide,worker,alloc,color,mid,core,total-pre-guide-worker-alloc-color-mid-core,total);
        out+=buf;
    }
    if(trace_job_timeline()) {
        std::lock_guard<std::mutex> guard(telemetryState.lock);
        const unsigned count=telemetryState.head<4?telemetryState.head:4;
        if(count) {
            out+=" | telemetry [";
            for(unsigned k=count;k>0;--k) {
                if(k!=count) out+=",";
                out+=telemetry_json(telemetryState.ring[(telemetryState.head-k)&15]);
            }
            out+="]";
        }
    }
    out+=" | dedupSkippedNotReady "+std::to_string(dedupSkippedNotReady.exchange(0,std::memory_order_relaxed));
    return out;
}
std::vector<JobTelemetry> take_job_telemetry() {
    std::lock_guard<std::mutex> guard(telemetryState.lock);
    const unsigned count=telemetryState.head<16?telemetryState.head:16;
    std::vector<JobTelemetry> out;
    out.reserve(count);
    for(unsigned k=count;k>0;--k) out.push_back(public_telemetry(telemetryState.ring[(telemetryState.head-k)&15]));
    return out;
}
std::string job_telemetry_json(const Job& job) {
    if(job.chronology[6]<=0) return {};
    return telemetry_json(telemetry_record(job));
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
    {
        std::lock_guard<std::mutex> guard(telemetryState.lock);
        auto& a=telemetryState.agg;
        const auto mean=[](TelemetryAcc& acc) {
            const double value=acc.n?acc.sum/acc.n:-1.0; acc={}; return value;
        };
        w.telemetryJobs=a.jobs; a.jobs=0;
        w.settleWaitMs=mean(a.settle); w.dedupWaitMs=mean(a.dedup);
        w.dedupGpuMs=mean(a.dedupGpu); w.ofaBootstrapMs=mean(a.ofaBoot);
        w.ofaRefineMs=mean(a.ofaRefine); w.guidesShaderMs=mean(a.guides);
        w.colorPrepMs=mean(a.prep); w.ngxPassMs=mean(a.ngx);
        w.stabilizeMs=mean(a.stab); w.colorRestoreMs=mean(a.restore); w.gpuMs=mean(a.gpu);
        w.preSubmitMs=mean(a.preSubmit); w.submitToDoneMs=mean(a.submitToDone);
        w.admitToDoneMs=mean(a.admitToDone);
        w.identityDeltaMaxMs=a.identityDeltaMax; a.identityDeltaMax=-1;
    }
    return w;
}
} // namespace nr::model
