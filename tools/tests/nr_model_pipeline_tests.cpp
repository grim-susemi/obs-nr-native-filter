// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - one live GPU pipeline fixture drives both the default 64x64 and the --1080p 60/120
// scenarios; a second translation unit would duplicate Host/Pipeline/prime and needs CMake outside this task.
#include "obs_native_output.h"
#include <obs.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <wrl/wrappers/corewrappers.h>

namespace {
using namespace nr;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
constexpr UINT w=640,h=384; // Default NR buffer (native extent) shared by both legacy modes.
constexpr UINT videoW=1920,videoH=1080; // 1080p mode OBS video frame; the NR buffer stays w x h.
constexpr uint64_t frame60Ns=16'666'667,frame120Ns=8'333'333;
struct JitterTiming {
    uint64_t intervalNs;
    int64_t shortNs,longNs;
    double shortLimitMs,longLimitMs;
    bool gated;
    const char* label;
};
constexpr JitterTiming jitter60{frame60Ns,9'000'000,24'000'000,12.5,20.0,true,"9ms/24ms"};
constexpr JitterTiming jitter120{frame120Ns,8'333'333,16'666'667,12.5,10.0,false,"8.33ms/16.67ms"};
const fs::path install=L"C:/Program Files/obs-studio/bin/64bit";
void expect(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
void check(HRESULT hr) { if(FAILED(hr))throw interop::HResultError(hr,"pipeline test hardware"); }
std::vector<std::byte> shader(const fs::path& path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    expect(in.good(),"missing built test shader");
    std::vector<std::byte> bytes(static_cast<size_t>(in.tellg()));
    in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    expect(in.good(),"read built shader");return bytes;
}
struct Host {
    const fs::path data=fs::temp_directory_path()/("nr-model-pipeline-"+std::to_string(GetCurrentProcessId()));
    explicit Host(UINT width=64,UINT height=64,UINT fps=60) {
        expect(fs::create_directory(data),"private test profile");
        expect(obs_startup("en-US",data.string().c_str(),nullptr),"standalone libobs clock");
        obs_video_info video{};const auto module=(install/"libobs-d3d11.dll").string();
        video.graphics_module=module.c_str();video.fps_num=fps;video.fps_den=1;
        video.base_width=video.output_width=width;video.base_height=video.output_height=height;
        video.output_format=VIDEO_FORMAT_RGBA;video.colorspace=VIDEO_CS_709;video.range=VIDEO_RANGE_FULL;
        const auto cwd=fs::current_path();fs::current_path(install); // libobs reads ../../data/libobs/*.effect
        const auto result=obs_reset_video(&video);fs::current_path(cwd);
        expect(result==OBS_VIDEO_SUCCESS,"libobs D3D11 clock");
    }
    ~Host() {obs_shutdown();std::error_code ec;fs::remove_all(data,ec);}
};
struct Faults final : model::ObservationSink {
    bool fault=false;
    std::shared_future<void> returned;
    model::Observation* reserve(const IngressSample&,const pipeline::TickPlan& plan) noexcept override {
        returned=plan.admission?plan.admission->returned:std::shared_future<void>{};return nullptr;
    }
    void capture(model::Observation*,ID3D11Texture2D*,ID3D11Texture2D*,ID3D11Texture2D*) noexcept override {}
    model::Fault qa_fault(const JobKey&) const noexcept override {
        return fault?model::Fault::CompletionSignal:model::Fault::None;
    }
};
struct Pipeline {
    static uint64_t next_instance() { static uint64_t next=51; return next++; }
    const uint64_t instance;
    const UINT bufferW,bufferH;
    const FrameDescriptor desc;
    const Settings settings;
    obs_adapter::NativeOutput output;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> graphics;
    ComPtr<ID3D11Texture2D> source,target,staging;
    std::shared_ptr<Faults> faults=std::make_shared<Faults>();
    std::vector<uint32_t> patternPixels;
    uint64_t tick=100,seq=0;
    std::vector<std::shared_ptr<model::Job>> jobs;
    Pipeline(const model::Config& config,const fs::path& assets,bool reuse,bool stabilize=false,
        uint32_t workScale=100,bool perfModel=false,UINT width=w,UINT height=h)
        :instance(next_instance()),bufferW(width),bufferH(height),patternPixels(static_cast<size_t>(width)*height),
         desc{{bufferW,bufferH},{0,0,bufferW,bufferH},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},
         settings{{1},true,config.runtime.canonicalPath.generic_string(),{1},core_default_tuning(),1,workScale,{},
            stabilize?StabilizeSettings{0.25f,0.10f,0.02f,0}:StabilizeSettings{},perfModel,reuse},
         output(instance,config,shader(assets/"nr_interop_vertex.cso"),shader(assets/"nr_interop_pixel.cso"),shader(assets/"nr_obs_analysis.cso")) {
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&graphics));
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC d{};
        check(device.As(&dxgi));check(dxgi->GetAdapter(&adapter));check(adapter->GetDesc(&d));
        std::printf("adapter=%ls vendor=%04x device=%04x hardware D3D11/12\n",d.Description,d.VendorId,d.DeviceId);
        D3D11_TEXTURE2D_DESC tex{};tex.Width=bufferW;tex.Height=bufferH;tex.MipLevels=tex.ArraySize=tex.SampleDesc.Count=1;
        tex.Format=DXGI_FORMAT_B8G8R8A8_UNORM;tex.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&tex,nullptr,&source));check(device->CreateTexture2D(&tex,nullptr,&target));
        tex.BindFlags=0;tex.Usage=D3D11_USAGE_STAGING;tex.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&tex,nullptr,&staging));output.observe_with(faults);
    }
    void pattern(unsigned shift,unsigned blue=0,unsigned overBand=0) {
        for(UINT i=0;i<bufferW*bufferH;++i) {
            const auto v=(i/bufferW*3+i%bufferW+shift)%192+32;
            patternPixels[i]=0xff000000u|((v+(i<overBand?4:0))<<16)|(v<<8)|(v+blue);
        }
        graphics->UpdateSubresource(source.Get(),0,nullptr,patternPixels.data(),bufferW*4,0);
    }
    Selection arrive() {
        ++tick;
        const auto decision=output.prepare({tick,2,3,SourceValidity::Valid,desc,settings},device.Get());
        expect(!decision.placeholder,"unexpected capture bypass");
        const auto& e=decision.epochs;
        auto selection=output.tick({{instance,e.contentEpoch,++seq},tick,tick-1,2,3,e.resourceGeneration,SourceValidity::Valid,desc,settings},
            source.Get(),target.Get());
        if(auto now=output.current_job();now && now->contract.key.frame.ingressSequence==seq)jobs.push_back(now);
        return selection;
    }
    std::shared_ptr<model::Job> current() {
        expect(!jobs.empty() && jobs.back()->contract.key.frame.ingressSequence==seq,"missing exact worker admission");
        return jobs.back();
    }
    void ready() {
        const auto job=current();
        expect(WaitForSingleObject(job->observed,30000)==WAIT_OBJECT_0,"original completion timeout");
        if(job->code>1)std::printf("job serial=%llu code=%x diagnostic=%s\n",job->contract.key.submissionSerial,job->code,job->diagnostic.c_str());
        expect(job->ready.load(),"missing original callback receipt");
        expect(faults->returned.valid() && faults->returned.wait_for(5s)==std::future_status::ready,"exact dispatch returned");
    }
    std::vector<uint32_t> presented() {
        graphics->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m{};
        check(graphics->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));
        std::vector<uint32_t> result(bufferW*bufferH);
        for(UINT y=0;y<bufferH;++y)std::memcpy(result.data()+y*bufferW,static_cast<const char*>(m.pData)+y*m.RowPitch,bufferW*4);
        graphics->Unmap(staging.Get(),0);return result;
    }
    void close() {
        jobs.clear(); // Release test-side immutable capture leases before the driver session is destroyed.
        const auto exited=output.worker_exit();
        output.request_stop();
        (void)output.try_close(); // Sends the pipeline stop once ownership is resolved; refusal is awaited below.
        expect(exited.valid() && exited.wait_for(10s)==std::future_status::ready,"pipeline worker exit timeout");
        expect(output.try_close(),"pipeline clean close");
        expect(output.closed(),"pipeline clean close");
    }
};
void prime(Pipeline& p) {
    for(unsigned n=0;n<12;++n) {
        p.pattern(9*n);p.arrive();p.ready();
        if(p.current()->eligible && p.current()->referenceValid)break;
    }
    const auto ordinary=p.current();
    expect(ordinary->evaluated && ordinary->referenceValid && ordinary->eligible,"real Feature18 + NVOF priming");
}
void reuse_and_refusal(Pipeline& p) {
    expect(_putenv_s("OBS_NR_TEST_DEDUP_SYNC","1")==0,"deterministic dedup fixture");
    prime(p);
    {
    const auto ordinary=p.current();
    const auto first=p.arrive();p.ready();
    expect(first.kind==OutputKind::DelayedNr && p.output.selected_job()==ordinary,"publish real ordinary result");
    const auto pixels=p.presented();const auto duplicate=p.current();
    expect(duplicate->reused && duplicate->eligible && !duplicate->evaluated &&
        duplicate->flow && !duplicate->flow->reset,"duplicate did not bypass NR evaluation with contiguous guides");
    expect(p.arrive().kind==OutputKind::DelayedNr && p.output.selected_job()==duplicate && p.presented()==pixels,
        "pixel-identical published duplicate");p.ready();
    const auto last=p.current();expect(last->reused,"consecutive duplicate");
    p.pattern(111);p.arrive();p.ready();
    auto resumed=p.current();
    expect(resumed->eligible && resumed->evaluated && resumed->flow && !resumed->flow->reset &&
        resumed->guideReferenceKey==last->contract.key && resumed->resetDiagnostic->previousKey==last->contract.key &&
        resumed->contract.key.historyEpoch==ordinary->contract.key.historyEpoch,"real guide and pass history after duplicates");
    std::printf("nr_reused=%u pixels_identical=%zu passes_skipped=2 next_reference=%llu history=%llu reset=0\n",
        2u,pixels.size(),last->contract.key.frame.ingressSequence,resumed->contract.key.historyEpoch);
    // 640*384*3 channels at 5 ppm admits 3 over-band components, not 4.
    p.pattern(111,3,3);p.arrive();p.ready();expect(p.current()->reused && p.current()->eligible,"inclusive band and ppm limit");
    p.pattern(111,3,4);p.arrive();p.ready();
    expect(!p.current()->reused && p.current()->eligible && p.current()->flow && !p.current()->flow->reset,"over-budget difference");
    std::puts("band_lsb=3 ppm=5 over_band=3:reuse,4:evaluate reset=0");
    // QA failure: an un-signalled comparison readback must skip reuse without a
    // worker stall, evaluate NR normally, and leave the next frame unaffected.
    expect(_putenv_s("OBS_NR_TEST_DEDUP_UNSIGNALLED","1")==0,"injected dedup silence");
    p.pattern(455);p.arrive();p.ready();
    p.pattern(455);p.arrive();p.ready();const auto skipped=p.current();
    expect(!skipped->reused && skipped->evaluated && skipped->eligible && skipped->flow &&
        (skipped->code==0||skipped->code==1),"injected silence did not fall back to evaluation");
    expect(skipped->dedupWaitQpc>=0 && model::qpc_ms(skipped->dedupWaitQpc)<0.2,
        "un-signalled dedup readback blocked the worker");
    const auto trace=model::take_job_trace();
    expect(trace.find("dedupSkippedNotReady")!=std::string::npos &&
        trace.find("dedupSkippedNotReady 0")==std::string::npos,"dedup skip not recorded");
    p.pattern(466);p.arrive();p.ready();
    expect(p.current()->eligible && p.current()->evaluated &&
        (p.current()->code==0||p.current()->code==1),"post-skip evaluation failed");
    expect(_putenv_s("OBS_NR_TEST_DEDUP_UNSIGNALLED","")==0,"dedup silence restore");
    }
    p.faults->fault=true;p.arrive();p.faults->fault=false;
    auto stuck=p.current();
    expect(p.faults->returned.valid() && p.faults->returned.wait_for(5s)==std::future_status::ready,"fault dispatch returned");
    expect(!stuck->ready && stuck->fault==model::Fault::CompletionSignal,"fault did not retain original");
    p.arrive();p.ready();
    expect(p.current()->code==0xe002 && !stuck->ready,"fault owner replaced or accepted");
    const auto& t=*p.current()->timeline;
    LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);
    const double ms=1000.0*double(t.settleEnd-p.current()->chronology[0])/double(hz.QuadPart);
    expect(ms<8,"fault waited on the ordinary settle budget");
    p.output.request_stop();
    expect(!p.output.try_close(),"unresolved owner closed");
    const auto retained=p.output.close_diagnostics();const auto point=stuck->completion();
    expect(retained.refusingSerial==stuck->contract.key.submissionSerial && retained.refusingGeneration==stuck->contract.key.resourceGeneration &&
        point.fence && point.fence->GetCompletedValue()<point.value && !stuck->ready &&
        stuck->originalMarker.state()==model::OriginalMarker::State::Held,"missing exact retained-close receipt");
    std::printf("fault=CompletionSignal 0xe002=1 settle_ms=%.3f retained_on_close=1 serial=%llu\n",ms,stuck->contract.key.submissionSerial);
    expect(p.output.release_original_marker(stuck->contract.key),"release original marker");
    expect(WaitForSingleObject(stuck->observed,5000)==WAIT_OBJECT_0 && stuck->ready,"original notification");
    stuck.reset();
    expect(_putenv_s("OBS_NR_TEST_DEDUP_SYNC","")==0,"dedup sync restore");
    p.close();
}
void dedup_proof(Pipeline& p) {
    expect(_putenv_s("OBS_NR_TEST_DEDUP_SYNC","1")==0,"deterministic dedup fixture");
    const auto ordinary=p.current();
    const auto first=p.arrive();p.ready();
    expect(first.kind==OutputKind::DelayedNr && p.output.selected_job()==ordinary,"1080p publish real ordinary result");
    const auto pixels=p.presented();const auto duplicate=p.current();
    expect(duplicate->reused && duplicate->eligible && !duplicate->evaluated &&
        duplicate->flow && !duplicate->flow->reset,"1080p duplicate bypassed NR evaluation with contiguous guides");
    expect(p.arrive().kind==OutputKind::DelayedNr && p.output.selected_job()==duplicate && p.presented()==pixels,
        "1080p pixel-identical published duplicate");p.ready();
    expect(p.current()->reused && !pixels.empty(),"1080p consecutive duplicate and identical pixels");
    std::printf("dedup nr_reused=%u pixels_identical=%zu nr_passes_skipped=2 guides_contiguous=1 reset=0 0xe002=0\n",2u,pixels.size());
    expect(_putenv_s("OBS_NR_TEST_DEDUP_SYNC","")==0,"dedup sync restore");
}
void jitter(Pipeline& p,const JitterTiming& timing) {
    prime(p);const auto history=p.current()->contract.key.historyEpoch;
    // Withhold the *real D3D11 producer*, not the model marker. A CPU producer
    // wait cannot return this dispatch while its GPU input still hasn't arrived.
    {
        ComPtr<ID3D12Fence> producer;const auto context=p.output.context();auto* device=context->device();
        check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&producer)));
        using namespace Microsoft::WRL::Wrappers;
        HandleT<HandleTraits::HANDLETraits> shared;
        check(device->CreateSharedHandle(producer.Get(),nullptr,GENERIC_ALL,nullptr,shared.GetAddressOf()));
        ComPtr<ID3D11Device5> d11;ComPtr<ID3D11DeviceContext4> graphics;ComPtr<ID3D11Fence> gate11;
        check(p.device.As(&d11));check(p.graphics.As(&graphics));check(d11->OpenSharedFence(shared.Get(),IID_PPV_ARGS(&gate11)));
        check(graphics->Wait(gate11.Get(),1));p.pattern(51);p.arrive();
        const bool early=p.faults->returned.wait_for(500ms)==std::future_status::ready;
        const bool incomplete=!p.current()->ready;
        check(producer->Signal(1));p.ready();
        expect(early && incomplete && p.current()->eligible,"producer wait blocked the model worker");
        std::puts("producer_gpu_wait=1 dispatch_returned_before_producer=1");
    }
    const auto start=p.jobs.size();
    const auto shortStep=std::chrono::nanoseconds(timing.shortNs),longStep=std::chrono::nanoseconds(timing.longNs);
    auto deadline=Clock::now();
    std::shared_ptr<model::Job> late;size_t shortHold=0;
    if(timing.gated) {
        ComPtr<ID3D12Fence> gate;
        check(p.output.context()->device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
        check(p.output.context()->queue()->Wait(gate.Get(),1)); // Real, unfinished GPU work at the next short cutoff.
        p.pattern(61);p.arrive();
        expect(p.faults->returned.wait_for(1s)==std::future_status::ready && !p.current()->ready,"gated model submission");
        const auto held=p.output.held_frames();const auto previousPixels=p.presented();
        std::stop_source stopped;stopped.request_stop();
        const auto before=Clock::now();
        expect(!p.current()->ready_for_arrival(stopped.get_token(),timing.intervalNs) && Clock::now()-before<8ms &&
            !p.current()->ready,"stopped healthy owner must not wait or become ready");
        deadline+=shortStep;std::this_thread::sleep_until(deadline);p.pattern(81);
        const auto selected=p.arrive();late=p.current();
        std::this_thread::sleep_for(3ms);check(gate->Signal(1));
        expect(selected.reason==Reason::DeadlineMiss && !selected.resetTemporal && p.output.held_frames()==held+1 &&
            p.presented()==previousPixels,
            "short-tick hold of last NR target");
        shortHold=previousPixels.size();
        for(unsigned n=1;n<20;++n) {
            deadline+=n%2?longStep:shortStep;std::this_thread::sleep_until(deadline);
            p.pattern(81+n*11);p.arrive();(void)p.current();
        }
    } else {
        deadline+=shortStep;std::this_thread::sleep_until(deadline);p.pattern(97);p.arrive();(void)p.current();
        for(unsigned n=1;n<21;++n) {
            deadline+=n%2?shortStep:longStep;std::this_thread::sleep_until(deadline);
            p.pattern(97+n*11);p.arrive();(void)p.current();if(n==1)late=p.current();
        }
    }
    p.ready();size_t misses=0;std::array<double,2> gaps{};int64_t prior=0;
    for(size_t n=start;n<p.jobs.size();++n) {
        const auto& j=p.jobs[n];expect(j->ready.load(),"jitter callback missing");
        misses+=j->code==0xe002;
        if(prior)gaps[(n-start-1)%2]+=model::qpc_ms(j->timeline->begin-prior);
        prior=j->timeline->begin;
        if(j->code==0xe002)std::printf("jitter unexpected 0xe002 serial=%llu pending=%llu\n",j->contract.key.submissionSerial,j->timeline->pendingSerial);
        expect(j->eligible && j->evaluated && j->flow && !j->flow->reset &&
            j->contract.key.historyEpoch==history,"jitter evaluation or history failure");
    }
    LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);
    const double settle=1000.0*double(late->timeline->settleEnd-late->chronology[0])/double(hz.QuadPart);
    if(timing.gated)
        std::printf("jitter=%s observed_ms=%.2f/%.2f intervals=20 frames=%zu 0xe002=%zu history_resets=0 short_hold_pixels=%zu gated_settle_ms=%.3f stopped_wait_bypassed=1\n",
            timing.label,gaps[0]/10,gaps[1]/10,p.jobs.size()-start,misses,shortHold,settle);
    else
        std::printf("jitter=%s observed_ms=%.2f/%.2f intervals=20 frames=%zu 0xe002=%zu history_resets=0 settle_ms=%.3f\n",
            timing.label,gaps[0]/10,gaps[1]/10,p.jobs.size()-start,misses,settle);
    expect((!timing.gated || settle>=1) && gaps[0]/10<timing.shortLimitMs && gaps[1]/10>timing.longLimitMs,"observed short/long arrivals and real settlement");
    expect(!misses && p.jobs.size()-start==21,"deadline reset during jitter");
    late.reset();p.close();
}
void jitter(Pipeline& p) { jitter(p,jitter60); }
void stabilization_proof(Pipeline& p) {
    prime(p);
    p.pattern(301);p.arrive();p.ready();
    p.pattern(313);p.arrive();p.ready();
    const auto& job=*p.current();
    expect(job.eligible && job.evaluated && job.gpuTiming.stabilizeMs>=0.0,
        "stabilize GPU stage not measured for its own JobKey");
    std::printf("stabilize_gpu_ms=%.3f\n",job.gpuTiming.stabilizeMs);
    p.close();
}
void timing_proof() {
    const auto window=model::take_job_timing();
    const auto entries=model::take_job_telemetry();
    const model::JobTelemetry* full=nullptr;
    for(const auto& entry:entries)
        if(std::strcmp(entry.kind,"evaluation")==0 && entry.gpu.ofaBootstrapMs>=0 &&
           entry.gpu.ofaRefineMs>=0 && entry.gpu.guidesShaderMs>=0 && entry.gpu.colorPrepMs>=0 &&
           entry.gpu.ngxPassMs>=0 && entry.gpu.stabilizeMs>=0 && entry.gpu.colorRestoreMs>=0) {
            full=&entry;break;
        }
    expect(full!=nullptr,"no single evaluation JobKey measured every GPU stage");
    const auto& job=*full;
    expect(std::fabs(job.admitToDoneMs-(job.preSubmitMs+job.submitToDoneMs))<=0.2,
        "per-job admit->done identity");
    expect(std::fabs(job.identityDeltaMs)<=0.2,"stored per-job identity delta");
    expect(job.gpu.gpuMs>=0,"per-job GPU total");
    expect(window.telemetryJobs>0 && window.settleWaitMs>=0 && window.dedupWaitMs>=0 &&
        window.dedupGpuMs>=0 && window.ofaBootstrapMs>=0 && window.ofaRefineMs>=0 &&
        window.guidesShaderMs>=0 && window.colorPrepMs>=0 && window.ngxPassMs>=0 &&
        window.stabilizeMs>=0 && window.colorRestoreMs>=0 && window.gpuMs>=0,
        "window stage means missing");
    expect(window.preSubmitMs>=0 && window.submitToDoneMs>=0 && window.admitToDoneMs>=0 &&
        window.identityDeltaMaxMs>=0 && window.identityDeltaMaxMs<=0.2,"window identity");
    const auto trace=model::take_job_trace();
    for(const char* field:{"\"settleWaitMs\"","\"dedupWaitMs\"","\"ofaBootstrapMs\"","\"ofaRefineMs\"",
        "\"guidesShaderMs\"","\"colorPrepMs\"","\"ngxPassMs\"","\"stabilizeMs\"","\"colorRestoreMs\"",
        "\"gpuMs\"","\"preSubmitMs\"","\"submitToDoneMs\"","\"admitToDoneMs\"","\"identityDeltaMs\""})
        expect(trace.find(field)!=std::string::npos,"telemetry log field missing");
    std::printf("NR_MODEL_TIMING {\"telemetryJobs\":%u,\"settleWaitMs\":%.3f,\"dedupWaitMs\":%.3f,"
        "\"dedupGpuMs\":%.3f,\"ofaBootstrapMs\":%.3f,\"ofaRefineMs\":%.3f,\"guidesShaderMs\":%.3f,"
        "\"colorPrepMs\":%.3f,\"ngxPassMs\":%.3f,\"stabilizeMs\":%.3f,\"colorRestoreMs\":%.3f,"
        "\"gpuMs\":%.3f,\"preSubmitMs\":%.3f,\"submitToDoneMs\":%.3f,\"admitToDoneMs\":%.3f,"
        "\"identityDeltaMaxMs\":%.4f}\n",
        window.telemetryJobs,window.settleWaitMs,window.dedupWaitMs,window.dedupGpuMs,window.ofaBootstrapMs,
        window.ofaRefineMs,window.guidesShaderMs,window.colorPrepMs,window.ngxPassMs,window.stabilizeMs,
        window.colorRestoreMs,window.gpuMs,window.preSubmitMs,window.submitToDoneMs,window.admitToDoneMs,
        window.identityDeltaMaxMs);
    std::printf("NR_MODEL_TIMING_JOB {\"instance\":%llu,\"gen\":%llu,\"serial\":%llu,\"ingress\":%llu,"
        "\"admitToDoneMs\":%.3f,\"preSubmitMs\":%.3f,\"submitToDoneMs\":%.3f,\"identityDeltaMs\":%.4f,"
        "\"gpuMs\":%.3f,\"ofaBootstrapMs\":%.3f,\"ofaRefineMs\":%.3f,\"guidesShaderMs\":%.3f,"
        "\"colorPrepMs\":%.3f,\"ngxPassMs\":%.3f,\"stabilizeMs\":%.3f,\"colorRestoreMs\":%.3f}\n",
        static_cast<unsigned long long>(job.instance),static_cast<unsigned long long>(job.generation),
        static_cast<unsigned long long>(job.serial),static_cast<unsigned long long>(job.ingress),
        job.admitToDoneMs,job.preSubmitMs,job.submitToDoneMs,job.identityDeltaMs,job.gpu.gpuMs,
        job.gpu.ofaBootstrapMs,job.gpu.ofaRefineMs,job.gpu.guidesShaderMs,job.gpu.colorPrepMs,
        job.gpu.ngxPassMs,job.gpu.stabilizeMs,job.gpu.colorRestoreMs);
}
void gpu_timer_fallback(Pipeline& p) {
    prime(p);
    p.pattern(421);p.arrive();p.ready();
    const auto& job=*p.current();
    expect(job.eligible && job.evaluated && job.code<=1,"fallback evaluation did not complete cleanly");
    expect(job.gpuTiming.gpuMs==-1.0 && job.gpuTiming.dedupGpuMs==-1.0 &&
        job.gpuTiming.ofaBootstrapMs==-1.0 && job.gpuTiming.ofaRefineMs==-1.0 &&
        job.gpuTiming.guidesShaderMs==-1.0 && job.gpuTiming.colorPrepMs==-1.0 &&
        job.gpuTiming.ngxPassMs==-1.0 && job.gpuTiming.stabilizeMs==-1.0 &&
        job.gpuTiming.colorRestoreMs==-1.0,"injected timer failure did not yield the -1.0 sentinel");
    const auto entries=model::take_job_telemetry();
    const model::JobTelemetry* found=nullptr;
    for(auto it=entries.rbegin();it!=entries.rend();++it)
        if(it->serial==job.contract.key.submissionSerial) { found=&*it; break; }
    expect(found && found->gpu.gpuMs==-1.0,"telemetry did not preserve the exact-job sentinel");
    std::printf("NR_MODEL_TIMING_FALLBACK {\"instance\":%llu,\"serial\":%llu,\"gpuMs\":%.1f,\"crashes\":0}\n",
        static_cast<unsigned long long>(job.contract.key.frame.instanceId),
        static_cast<unsigned long long>(job.contract.key.submissionSerial),job.gpuTiming.gpuMs);
    p.close();
}
}
std::shared_ptr<model::Job> ready_job(const std::shared_ptr<model::Job>& job) {
    expect(WaitForSingleObject(job->observed,30000)==WAIT_OBJECT_0,"perf completion timeout");
    expect(job->ready.load(std::memory_order_acquire),"perf completion callback");
    return job;
}
// High-resolution absolute cadence: the default Sleep granularity (~15.6 ms) lets a
// 60 Hz synthetic tick drift, which shows up as deadline misses unrelated to NR.
void wait_until(Clock::time_point deadline) {
    static HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    expect(timer!=nullptr,"high-resolution cadence timer");
    for(;;) {
        const auto now=Clock::now();
        if(now>=deadline)return;
        const auto remain=std::chrono::duration_cast<std::chrono::microseconds>(deadline-now).count();
        if(remain<=1200) {YieldProcessor();continue;}
        LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>((remain-1000)*10);
        expect(SetWaitableTimer(timer,&due,0,nullptr,nullptr,0)!=FALSE,"cadence timer arm");
        expect(WaitForSingleObject(timer,INFINITE)==WAIT_OBJECT_0,"cadence timer wait");
    }
}
double percentile(std::vector<double> values,double q) {
    if(values.empty())return -1.0;
    std::sort(values.begin(),values.end());
    const auto index=std::min(values.size()-1,static_cast<size_t>(q*static_cast<double>(values.size())));
    return values[index];
}
struct PerfCase {
    uint32_t scale=100;
    bool stabilize=false,modelB=true,enforce=false,loaded=false;
};
// Real 1440p NR+Model B lane with an explicit duplicate burst. The gate drives one
// complete frame per interval (work latency only); the loaded audit keeps one job
// outstanding so the settle/display cadence is visible too. Metrics come from the
// production Job fields, not from the test clock; no frame is dropped.
struct PerfTrial {
    double p50=0,p95=0,p99=0,max=0,workerP95=0,queueP95=0,settleP95=0,dedupP95=0,dedupMax=0,applied=1;
    double gpu=-1,ofaBoot=-1,ofaRefine=-1,guides=-1,ngx=-1,colorPrep=-1,stab=-1;
    size_t samples=0,dedupSamples=0,considered=0,misses=0,code2=0,evaluated=0,reused=0,eligible=0,published=0;
};
double median3(std::array<double,3> v) {std::sort(v.begin(),v.end());return v[1];}
// Real 1440p NR+Model B lane with an explicit duplicate burst. Three independent
// steady bursts are summarised by their median (the project's benchmark rule); every
// trial still reports its own p95, applied denominator, and evaluated count, so no
// trial may hide dropped NR frames. The loaded audit keeps one job outstanding so the
// settle/display cadence is visible; the gate drives one complete frame per interval.
void perf_lane(const model::Config& c,const fs::path& assets,const PerfCase& kase) {
    constexpr int frames=48,trials=3;
    constexpr int duplicateFirst=20,duplicateLast=23;
    Pipeline p(c,assets,true,kase.stabilize,kase.scale,kase.modelB,2560,1440);
    prime(p);
    const char* runtime="?";
    for(const auto& variant:runtime::allowed_runtimes())
        if(variant.sha256==c.runtime.sha256) runtime=variant.version.data();
    const int moving=frames-(duplicateLast-duplicateFirst+1);
    std::array<PerfTrial,trials> results{};
    for(int trial=0;trial<trials;++trial) {
        std::vector<std::shared_ptr<model::Job>> measured;
        std::vector<Selection> selections;
        auto deadline=Clock::now();
        for(int n=0;n<frames;++n) {
            deadline+=std::chrono::nanoseconds(frame60Ns);
            wait_until(deadline);
            if(n>=duplicateFirst && n<=duplicateLast) p.pattern(777+trial*13,0,0);
            else p.pattern(static_cast<unsigned>(31*n+5+trial*997),0,0);
            selections.push_back(p.arrive());
            measured.push_back(p.current());
            if(kase.loaded) {if(measured.size()>1) ready_job(measured[measured.size()-2]);}
            else ready_job(measured.back());
        }
        for(auto& job:measured) if(!job->ready.load(std::memory_order_acquire)) ready_job(job);
        const size_t considered=measured.size()>2?measured.size()-2:0;
        std::vector<double> admits,workers,queues,settles,dedups,gpus;
        double admitMax=0.0,dedupMax=0.0;
        double ofaBoot=0.0,ofaRefine=0.0,guidesMs=0.0,ngxMs=0.0,prepMs=0.0,stabMs=0.0;size_t stageN=0;
        auto& r=results[trial];r.considered=considered;
        for(size_t i=0;i<considered;++i) {
            const auto& job=*measured[i];
            expect(job.chronology[6]>0 && job.admittedQpc>0,"perf job missing completion stamps");
            admits.push_back(model::qpc_ms(job.chronology[6]-job.admittedQpc));
            if(admits.size()==1 || admits.back()>admitMax) admitMax=admits.back();
            workers.push_back(model::qpc_ms(job.chronology[6]-job.dequeuedQpc));
            queues.push_back(model::qpc_ms(job.dequeuedQpc-job.admittedQpc));
            settles.push_back(model::qpc_ms(job.settleWaitQpc));
            if(job.dedupWaitQpc>=0) {
                dedups.push_back(model::qpc_ms(job.dedupWaitQpc));
                if(dedups.size()==1 || dedups.back()>dedupMax) dedupMax=dedups.back();
            }
            if(job.gpuTiming.gpuMs>=0.0) gpus.push_back(job.gpuTiming.gpuMs);
            if(job.evaluated && job.gpuTiming.ofaBootstrapMs>=0.0 && job.gpuTiming.ngxPassMs>=0.0) {
                ofaBoot+=job.gpuTiming.ofaBootstrapMs;ofaRefine+=job.gpuTiming.ofaRefineMs;
                guidesMs+=job.gpuTiming.guidesShaderMs;ngxMs+=job.gpuTiming.ngxPassMs;
                prepMs+=job.gpuTiming.colorPrepMs;stabMs+=job.gpuTiming.stabilizeMs>=0.0?job.gpuTiming.stabilizeMs:0.0;
                ++stageN;
            }
            if(job.code==0xe002)++r.code2;
            if(job.reused)++r.reused;
            if(job.evaluated)++r.evaluated;
            if(job.eligible)++r.eligible;
            if(job.published)++r.published;
        }
        for(const auto& selected:selections) if(selected.reason==Reason::DeadlineMiss) ++r.misses;
        r.applied=r.eligible?static_cast<double>(r.published)/static_cast<double>(r.eligible):1.0;
        r.p50=percentile(admits,0.50);r.p95=percentile(admits,0.95);r.p99=percentile(admits,0.99);r.max=admitMax;
        r.workerP95=percentile(workers,0.95);r.queueP95=percentile(queues,0.95);
        r.settleP95=percentile(settles,0.95);r.samples=dedups.size();r.dedupP95=percentile(dedups,0.95);r.dedupMax=dedupMax;
        r.gpu=gpus.empty()?-1.0:std::accumulate(gpus.begin(),gpus.end(),0.0)/static_cast<double>(gpus.size());
        r.ofaBoot=stageN?ofaBoot/stageN:-1.0;r.ofaRefine=stageN?ofaRefine/stageN:-1.0;
        r.guides=stageN?guidesMs/stageN:-1.0;r.ngx=stageN?ngxMs/stageN:-1.0;
        r.colorPrep=stageN?prepMs/stageN:-1.0;r.stab=stageN?stabMs/stageN:-1.0;
        std::printf("NR_PERF_TRIAL {\"scale\":%u,\"stabilize\":%s,\"model\":\"%s\",\"trial\":%d,"
            "\"admitP50Ms\":%.3f,\"admitP95Ms\":%.3f,\"admitP99Ms\":%.3f,\"admitMaxMs\":%.3f,"
            "\"workerP95Ms\":%.3f,\"queueP95Ms\":%.3f,\"settleP95Ms\":%.3f,\"dedupSamples\":%zu,\"dedupP95Ms\":%.3f,\"dedupMaxMs\":%.3f,"
            "\"deadlineMiss\":%zu,\"code0xe002\":%zu,\"evaluated\":%zu,\"reused\":%zu,"
            "\"eligible\":%zu,\"published\":%zu,\"applied\":%.4f}\n",
            kase.scale,kase.stabilize?"true":"false",kase.modelB?"B":"A",trial,
            r.p50,r.p95,r.p99,r.max,r.workerP95,r.queueP95,r.settleP95,r.samples,r.dedupP95,r.dedupMax,
            r.misses,r.code2,r.evaluated,r.reused,r.eligible,r.published,r.applied);
        measured.clear();selections.clear();p.jobs.clear();
    }
    const auto med=[&](const auto& get) {return median3({get(results[0]),get(results[1]),get(results[2])});};
    const double p95=med([](const PerfTrial& t){return t.p95;});
    const double p99=med([](const PerfTrial& t){return t.p99;});
    const double p50=med([](const PerfTrial& t){return t.p50;});
    const double worker=med([](const PerfTrial& t){return t.workerP95;});
    const double dedup=med([](const PerfTrial& t){return t.dedupP95;});
    const double misses=med([](const PerfTrial& t){return static_cast<double>(t.misses);});
    const double applied=med([](const PerfTrial& t){return t.applied;});
    size_t minEval=results[0].evaluated,maxReuse=0,minSamples=results[0].samples,maxCode2=0;
    for(const auto& t:results) {
        minEval=std::min(minEval,t.evaluated);maxReuse=std::max(maxReuse,t.reused);
        minSamples=std::min(minSamples,t.samples);maxCode2=std::max(maxCode2,t.code2);
    }
    std::printf("NR_PERF_LANE {\"scale\":%u,\"stabilize\":%s,\"model\":\"%s\",\"runtime\":\"%s\","
        "\"trials\":%d,\"frames\":%d,\"considered\":%zu,\"moving\":%d,"
        "\"admitP50Ms\":%.3f,\"admitP95Ms\":%.3f,\"admitP99Ms\":%.3f,"
        "\"workerP95Ms\":%.3f,\"queueP95Ms\":%.3f,"
        "\"p95Trials\":[%.3f,%.3f,%.3f],\"missTrials\":[%zu,%zu,%zu],\"dedupP95Trials\":[%.3f,%.3f,%.3f],"
        "\"appliedTrials\":[%.4f,%.4f,%.4f],\"evaluatedTrials\":[%zu,%zu,%zu],\"reusedTrials\":[%zu,%zu,%zu],"
        "\"dedupSamples\":%zu,\"dedupP95Ms\":%.3f,\"deadlineMiss\":%.0f,\"code0xe002\":%zu,"
        "\"evaluated\":%zu,\"reused\":%zu,\"applied\":%.4f,\"gpuMeanMs\":%.3f,"
        "\"ofaBootstrapMs\":%.3f,\"ofaRefineMs\":%.3f,\"guidesShaderMs\":%.3f,\"ngxPassMs\":%.3f,"
        "\"colorPrepMs\":%.3f,\"stabilizeMs\":%.3f}\n",
        kase.scale,kase.stabilize?"true":"false",kase.modelB?"B":"A",runtime,trials,frames,
        results[0].considered,moving,p50,p95,p99,worker,med([](const PerfTrial& t){return t.queueP95;}),
        results[0].p95,results[1].p95,results[2].p95,
        results[0].misses,results[1].misses,results[2].misses,
        results[0].dedupP95,results[1].dedupP95,results[2].dedupP95,
        results[0].applied,results[1].applied,results[2].applied,
        results[0].evaluated,results[1].evaluated,results[2].evaluated,
        results[0].reused,results[1].reused,results[2].reused,
        minSamples,dedup,misses,maxCode2,minEval,maxReuse,applied,
        med([](const PerfTrial& t){return t.gpu;}),med([](const PerfTrial& t){return t.ofaBoot;}),
        med([](const PerfTrial& t){return t.ofaRefine;}),med([](const PerfTrial& t){return t.guides;}),
        med([](const PerfTrial& t){return t.ngx;}),med([](const PerfTrial& t){return t.colorPrep;}),
        med([](const PerfTrial& t){return t.stab;}));
    if(kase.enforce) {
        expect(kase.modelB && p.settings.dedupReuse && !kase.stabilize,"budget gate covers Model B + dedup reuse only");
        // The worker is the unit under test: the queue split is the caller's
        // capture/display thread, which the NR pre-submit cannot influence. Both
        // numbers are emitted; only the worker latency is gated here.
        expect(worker<=16.67,"1440p worker median p95 over one frame interval");
        expect(minSamples>=2,"dedup path not exercised");
        expect(dedup<0.2,"dedup readback stalled the worker");
        expect(misses==0,"deadline miss census");
        expect(maxCode2==0,"worker evaluation failure census");
        expect(applied>=0.99,"applied denominator below steady publication");
        expect(minEval>=static_cast<size_t>(moving-1),"NR frames dropped to lower a percentile");
        expect(maxReuse<=static_cast<size_t>(duplicateLast-duplicateFirst+1),"dedup reuse rate inflated");
    }
    p.jobs.clear();
    p.close();
}
model::Config config(const Host& host,const fs::path& assets,uint64_t intervalNs) {
    model::Config c;
    // The installed allowlisted variant may be either pinned NR build (310.8.0.0 or
    // 310.9.16.0); select by the actual bytes instead of assuming the oldest entry.
    // Module::open still verifies that exact hash and the NVIDIA signature.
    const auto runtimePath=fs::canonical(install/"nvngx_dlssnr.dll");
    const runtime::RuntimeVariant* installed=nullptr;
    for(const auto& variant:runtime::allowed_runtimes())
        if(runtime::inspect({runtimePath,std::string(variant.sha256)}).accepted){installed=&variant;break;}
    expect(installed!=nullptr,"installed NR runtime is an allowlisted variant");
    c.runtime={runtimePath,std::string(installed->sha256)};
    c.driver=runtime::resolve_driver().identity.canonicalPath;c.data=fs::canonical(host.data);
    c.codec=shader(assets/"nr_codec.dxil");c.color=shader(assets/"nr_color.dxil");c.guides=shader(assets/"nr_guides.cso");
    c.stabilize=shader(assets/"nr_stabilize.dxil");
    c.frameIntervalNs=intervalNs;return c;
}
int main(int argc,char** argv) try {
    std::setvbuf(stdout,nullptr,_IONBF,0);expect(_putenv_s("OBS_NR_JOB_TIMELINE","1")==0,"timeline environment");
    std::array<wchar_t,32768> exe{};expect(GetModuleFileNameW(nullptr,exe.data(),DWORD(exe.size()))!=0,"test location");
    const auto assets=fs::path(exe.data()).parent_path().parent_path();
    const bool mode1080p=argc>1 && std::string(argv[1])=="--1080p";
    const std::string mode=argc>1?std::string(argv[1]):std::string{};
    const bool mode1440p=mode=="--1440p" || mode=="--1440p-baseline";
    const bool modeAudit=mode=="--1440p-audit";
    if(mode1440p || modeAudit) {
        const bool enforce=mode=="--1440p";
        Host host(2560,1440,60);
        const auto c=config(host,assets,frame60Ns);
        const bool modelB=runtime::supports_model_b(c.runtime.sha256);
        if(enforce && !modelB) throw std::runtime_error("installed NR runtime cannot select Model B");
        if(modeAudit) {
            for(const bool perf:{false,true}) for(const auto scale:{100u,85u,75u})
                perf_lane(c,assets,PerfCase{scale,false,perf,false,true});
            std::puts("PASS nr_model_pipeline 1440p Model A/B audit");
            return 0;
        }
        const bool loaded=mode=="--1440p-baseline";
        for(const auto scale:{100u,75u}) {
            perf_lane(c,assets,PerfCase{scale,false,true,enforce,loaded});
            if(scale==75u) perf_lane(c,assets,PerfCase{scale,true,true,false,loaded});
        }
        if(!enforce) std::puts("NR_PERF_BASELINE_ONLY budgets not asserted");
        std::puts("PASS nr_model_pipeline 1440p Model B");
        return 0;
    }
    if(mode1080p) {
        // Real 1920x1080 OBS video frame with the NR buffer fixed at 640x384.
        for(const unsigned fps: {60u,120u}) {
            Host host(videoW,videoH,fps);
            const auto timing=fps==60u?jitter60:jitter120;
            const auto c=config(host,assets,timing.intervalNs);
            {Pipeline p(c,assets,false);jitter(p,timing);}
            {Pipeline p(c,assets,true);prime(p);dedup_proof(p);p.close();}
            std::printf("mode=1080p%u video=%ux%u nr=%ux%u transport_interval_ns=%llu 0xe002=0 history_resets=0\n",
                fps,videoW,videoH,w,h,static_cast<unsigned long long>(timing.intervalNs));
        }
    } else {
        Host host;const auto c=config(host,assets,frame60Ns);
        {Pipeline p(c,assets,false);jitter(p);}
        {Pipeline p(c,assets,true);reuse_and_refusal(p);}
        {Pipeline p(c,assets,false,true);stabilization_proof(p);}
        timing_proof();
        expect(_putenv_s("OBS_NR_TEST_GPU_TIMER_FAIL","1")==0,"timer fault environment");
        {Pipeline p(c,assets,false);gpu_timer_fallback(p);}
        expect(_putenv_s("OBS_NR_TEST_GPU_TIMER_FAIL","")==0,"timer fault environment restore");
    }
    std::puts("PASS nr_model_pipeline");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL nr_model_pipeline: %s\n",e.what());return 1;}
