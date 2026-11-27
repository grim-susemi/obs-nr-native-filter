// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - one live GPU pipeline fixture drives both the default 64x64 and the --1080p 60/120
// scenarios; a second translation unit would duplicate Host/Pipeline/prime and needs CMake outside this task.
#include "obs_native_output.h"
#include <obs.h>
#include <chrono>
#include <cstdio>
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
constexpr UINT w=640,h=384; // NR buffer (native extent) shared by both modes.
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
    const FrameDescriptor desc{{w,h},{0,0,w,h},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80};
    const Settings settings;
    obs_adapter::NativeOutput output;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> graphics;
    ComPtr<ID3D11Texture2D> source,target,staging;
    std::shared_ptr<Faults> faults=std::make_shared<Faults>();
    uint64_t tick=100,seq=0;
    std::vector<std::shared_ptr<model::Job>> jobs;
    Pipeline(const model::Config& config,const fs::path& assets,bool reuse)
        :settings{{1},true,config.runtime.canonicalPath.generic_string(),{1},core_default_tuning(),1,100,{}, {},false,reuse},
         output(51,config,shader(assets/"nr_interop_vertex.cso"),shader(assets/"nr_interop_pixel.cso"),shader(assets/"nr_obs_analysis.cso")) {
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&graphics));
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC d{};
        check(device.As(&dxgi));check(dxgi->GetAdapter(&adapter));check(adapter->GetDesc(&d));
        std::printf("adapter=%ls vendor=%04x device=%04x hardware D3D11/12\n",d.Description,d.VendorId,d.DeviceId);
        D3D11_TEXTURE2D_DESC tex{};tex.Width=w;tex.Height=h;tex.MipLevels=tex.ArraySize=tex.SampleDesc.Count=1;
        tex.Format=DXGI_FORMAT_B8G8R8A8_UNORM;tex.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&tex,nullptr,&source));check(device->CreateTexture2D(&tex,nullptr,&target));
        tex.BindFlags=0;tex.Usage=D3D11_USAGE_STAGING;tex.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&tex,nullptr,&staging));output.observe_with(faults);
    }
    void pattern(unsigned shift,unsigned blue=0,unsigned overBand=0) {
        std::vector<uint32_t> pixels(w*h);
        for(UINT i=0;i<w*h;++i) {
            const auto v=(i/w*3+i%w+shift)%192+32;
            pixels[i]=0xff000000u|((v+(i<overBand?4:0))<<16)|(v<<8)|(v+blue);
        }
        graphics->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),w*4,0);
    }
    Selection arrive() {
        ++tick;
        const auto decision=output.prepare({tick,2,3,SourceValidity::Valid,desc,settings},device.Get());
        expect(!decision.placeholder,"unexpected capture bypass");
        const auto& e=decision.epochs;
        auto selection=output.tick({{51,e.contentEpoch,++seq},tick,tick-1,2,3,e.resourceGeneration,SourceValidity::Valid,desc,settings},
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
        std::vector<uint32_t> result(w*h);
        for(UINT y=0;y<h;++y)std::memcpy(result.data()+y*w,static_cast<const char*>(m.pData)+y*m.RowPitch,w*4);
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
    prime(p);
    {
    const auto ordinary=p.current();
    const auto first=p.arrive();p.ready();
    expect(first.kind==OutputKind::DelayedNr && p.output.selected_job()==ordinary,"publish real ordinary result");
    const auto pixels=p.presented();const auto duplicate=p.current();
    expect(duplicate->reused && duplicate->eligible && !duplicate->evaluated && !duplicate->flow,"duplicate bypassed both engines");
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
    stuck.reset();p.close();
}
void dedup_proof(Pipeline& p) {
    const auto ordinary=p.current();
    const auto first=p.arrive();p.ready();
    expect(first.kind==OutputKind::DelayedNr && p.output.selected_job()==ordinary,"1080p publish real ordinary result");
    const auto pixels=p.presented();const auto duplicate=p.current();
    expect(duplicate->reused && duplicate->eligible && !duplicate->evaluated && !duplicate->flow,
        "1080p duplicate bypassed both engines");
    expect(p.arrive().kind==OutputKind::DelayedNr && p.output.selected_job()==duplicate && p.presented()==pixels,
        "1080p pixel-identical published duplicate");p.ready();
    expect(p.current()->reused && !pixels.empty(),"1080p consecutive duplicate and identical pixels");
    std::printf("dedup nr_reused=%u pixels_identical=%zu passes_skipped=2 reset=0 0xe002=0\n",2u,pixels.size());
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
}
model::Config config(const Host& host,const fs::path& assets,uint64_t intervalNs) {
    model::Config c;c.runtime={fs::canonical(install/"nvngx_dlssnr.dll"),std::string(runtime::allowed_runtimes()[0].sha256)};
    c.driver=runtime::resolve_driver().identity.canonicalPath;c.data=fs::canonical(host.data);
    c.codec=shader(assets/"nr_codec.dxil");c.color=shader(assets/"nr_color.dxil");c.guides=shader(assets/"nr_guides.cso");
    c.frameIntervalNs=intervalNs;return c;
}
int main(int argc,char** argv) try {
    std::setvbuf(stdout,nullptr,_IONBF,0);expect(_putenv_s("OBS_NR_JOB_TIMELINE","1")==0,"timeline environment");
    std::array<wchar_t,32768> exe{};expect(GetModuleFileNameW(nullptr,exe.data(),DWORD(exe.size()))!=0,"test location");
    const auto assets=fs::path(exe.data()).parent_path().parent_path();
    const bool mode1080p=argc>1 && std::string(argv[1])=="--1080p";
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
    }
    std::puts("PASS nr_model_pipeline");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL nr_model_pipeline: %s\n",e.what());return 1;}
