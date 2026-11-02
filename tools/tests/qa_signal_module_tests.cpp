// SPDX-License-Identifier: GPL-3.0-only
#include "nr_model.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
using namespace nr;
using Signals=std::array<std::shared_ptr<model::QaSignal>,3>;

#ifdef NR_QA_SIGNAL_MODULE_DLL
namespace {
struct Owner final:pipeline::Processor {
    std::promise<void> entered,dependency;
    const std::shared_future<void> started=entered.get_future().share();
    const std::shared_future<void> released=dependency.get_future().share();
    std::atomic<unsigned> stopped{0};
    pipeline::Processed process(const pipeline::Work& work,std::stop_token) override {
        entered.set_value();
        if(released.wait_for(std::chrono::seconds(5))!=std::future_status::ready)
            throw std::runtime_error("module probe exact dependency");
        return {work.job,false,{}}; // Actual scheduler return, not model/GPU proof.
    }
    void worker_stopping() noexcept override {stopped.fetch_add(1,std::memory_order_release);}
};
struct Probe {
    std::shared_ptr<Owner> owner=std::make_shared<Owner>();
    std::unique_ptr<pipeline::FramePipeline> pipeline=
        std::make_unique<pipeline::FramePipeline>(Epochs{701,1,1,1},owner);
    Signals signals{model::make_qa_signal(),model::make_qa_signal(),model::make_qa_signal()};
    std::optional<interop::Ticket> ticket;
    Probe() {
        signals[0]->bind(owner->started);signals[2]->bind(pipeline->worker_exit());
        const Settings settings{{1},true,"C:/qa/not-loaded-runtime.dll",{1},{0,0,1,1,0,-1,true}};
        const IngressSample input{{701,1,1},1000,999,31,91,1,SourceValidity::Valid,
            {{1920,1080},{0,0,1920,1080},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Straight,80},settings};
        const auto plan=pipeline->begin(input,1001);
        if(!plan.admission)throw std::runtime_error("module probe actual admission");
        signals[1]->bind(plan.admission->returned); // Before real production dispatch.
        ticket.emplace(interop::Ticket{plan.admission->job.key,plan.admission->slot});
        const interop::Ownership physical{ticket->key,interop::WorkPhase::Captured,1,0,0,1,0,0,false,false,false};
        const pipeline::WorkBinding binding{*ticket,physical,{},owner};
        const auto raw=std::make_shared<const pipeline::RawFrame>(
            pipeline::RawFrame{input,std::make_shared<const unsigned>(1)});
        if(!pipeline->finish(raw,&binding))throw std::runtime_error("module probe actual finish");
    }
    bool retire() {
        if(!signals[1]->ready())return false;
        const interop::Ownership discarded{ticket->key,interop::WorkPhase::Discarded,1,0,0,1,0,0,false,false,false};
        if(!pipeline->observe(*ticket,discarded)||!pipeline->cancel(ticket->key))return false;
        pipeline->collect();pipeline->request_stop();
        if(!signals[2]->wait_for(5000)||owner->stopped.load(std::memory_order_acquire)!=1||!pipeline->try_close())return false;
        pipeline.reset();
        return true;
    }
};
}
extern "C" __declspec(dllexport) void* nr_qa_signal_probe_create(Signals* out) {
    try {
        auto probe=std::make_unique<Probe>();*out=probe->signals;return probe.release();
    }catch(...){return nullptr;}
}
extern "C" __declspec(dllexport) void nr_qa_signal_probe_release(void* value) {
    static_cast<Probe*>(value)->owner->dependency.set_value();
}
extern "C" __declspec(dllexport) bool nr_qa_signal_probe_retire(void* value) {
    try{return static_cast<Probe*>(value)->retire();}catch(...){return false;}
}
extern "C" __declspec(dllexport) void nr_qa_signal_probe_destroy(void* value) {delete static_cast<Probe*>(value);}
extern "C" __declspec(dllexport) uint64_t nr_qa_signal_probe_live() {return model::qa_signal_live_count();}
extern "C" __declspec(dllexport) bool nr_qa_signal_probe_heap() {
#ifdef _DEBUG
    return _CrtCheckMemory()!=FALSE;
#else
    return true;
#endif
}
#else
namespace {
unsigned checks=0;
void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class T>T function(HMODULE module,const char* name) {
    const auto address=GetProcAddress(module,name);expect(address!=nullptr,"module probe export");
    return reinterpret_cast<T>(address);
}
}
int wmain(int argc,wchar_t** argv)try {
    expect(argc==2,"exact module probe path");
    const auto module=LoadLibraryW(argv[1]);expect(module!=nullptr,"module probe load");
    const auto create=function<void*(*)(Signals*)>(module,"nr_qa_signal_probe_create");
    const auto release=function<void(*)(void*)>(module,"nr_qa_signal_probe_release");
    const auto retire=function<bool(*)(void*)>(module,"nr_qa_signal_probe_retire");
    const auto destroy=function<void(*)(void*)>(module,"nr_qa_signal_probe_destroy");
    const auto live=function<uint64_t(*)()>(module,"nr_qa_signal_probe_live");
    const auto heap=function<bool(*)()>(module,"nr_qa_signal_probe_heap");
    expect(live()==0&&heap(),"module starts without signal owners/heap damage");
    for(const auto order:std::array<std::array<size_t,3>,6>{{{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}}}) {
        Signals signals;auto* probe=create(&signals);expect(probe!=nullptr,"actual production signal probe");
        for(const auto& signal:signals)
            expect(signal&&signal->owner_module()==reinterpret_cast<uintptr_t>(module),"signal owner is actual loaded module");
        expect(signals[0]->wait_for(5000),"exact entered subscription");
        expect(!signals[1]->ready()&&!signals[2]->ready(),"return/exit cannot be manufactured before dependency");
        release(probe);expect(signals[1]->wait_for(5000),"real production dispatch returned");
        expect(retire(probe),"real production worker exit and pipeline retirement");
        destroy(probe); // Drop all module-side owners BEFORE the host's final references.
        expect(live()==3,"host now owns the final opaque references");
        auto duplicate=signals[order[0]];signals[order[0]].reset();
        expect(live()==3&&duplicate->ready(),"nonfinal host release retains module signal");
        duplicate.reset();expect(live()==2&&heap(),"final host release uses module destructor");
        signals[order[1]].reset();expect(live()==1&&heap(),"second final reference retirement");
        signals[order[2]].reset();expect(live()==0&&heap(),"last future state retired in module");
    }
    expect(FreeLibrary(module)!=FALSE,"module unload after final ownership release");
    std::printf("PASS qa_signal_module checks=%u production_dispatch=true production_exit=true final_host_reference=true CPU_only=true\n",checks);
    return 0;
}catch(const std::exception& error){std::puts(error.what());return 1;}
#endif
