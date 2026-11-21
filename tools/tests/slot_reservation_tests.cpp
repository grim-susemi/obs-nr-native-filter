// SPDX-License-Identifier: GPL-3.0-only
#include "frame_pipeline.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace nr;
using namespace nr::pipeline;
namespace {
void expect(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
void await(const std::shared_future<void>& event) {
    expect(event.wait_for(std::chrono::seconds(5))==std::future_status::ready,"worker event timeout");
    event.get();
}
std::atomic<int64_t> clockValue{1};
int64_t diagnostic_clock() { return clockValue.fetch_add(1); }
struct Copy final:Processor {
    std::array<int64_t,2> lastTiming{};
    Processed process(const Work& w,std::stop_token) override {
        lastTiming={w.enqueued,w.dequeued};
        return {w.job,true,w.raw->image};
    }
};
IngressSample input(const Epochs& e,uint64_t seq,uint64_t tick) {
    return {{e.instanceId,e.contentEpoch,seq},tick,tick-1,10,20,e.resourceGeneration,SourceValidity::Valid,
        {{1920,1080},{0,0,1920,1080},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},
        {{1},true,"C:/readonly/not-loaded.dll",{1},{0,0,1,1,0,-1,true}}};
}
interop::Ownership receipt(const Admission& a,uint64_t consumer,uint64_t done,bool open) {
    const auto producer=a.job.key.submissionSerial;
    return {a.job.key,interop::WorkPhase::Submitted,producer,1,consumer,producer,1,done,open,false,false};
}
std::shared_ptr<const RawFrame> raw(const IngressSample& sample) {
    return std::make_shared<const RawFrame>(RawFrame{sample,std::make_shared<const uint64_t>(sample.key.ingressSequence)});
}
void run(bool refused) {
    auto processor=std::make_shared<Copy>();
    FramePipeline pipeline({41,1,1,1},processor);
    std::vector<Admission> jobs;
    for(uint64_t seq=1;seq<=2;++seq) {
        const auto sample=input(pipeline.epochs(),seq,99+seq);
        const auto plan=pipeline.begin(sample,100+seq);
        expect(plan.admission && plan.admission->slot==seq-1,"initial reservation");
        const auto& a=*plan.admission;
        const interop::Ownership captured{a.job.key,interop::WorkPhase::Captured,1,0,0,0,0,0,false,false,false};
        const auto expected=clockValue.load();
        const WorkBinding binding{{a.job.key,a.slot},captured,{},processor,{},{},diagnostic_clock};
        expect(pipeline.finish(raw(sample),&binding),"initial dispatch");
        await(a.returned);jobs.push_back(a);
        expect(processor->lastTiming==std::array<int64_t,2>{expected,expected+1},
            "exact work must carry its own enqueue and dequeue stamps");
        expect(pipeline.observe({a.job.key,a.slot},receipt(a,seq,1,seq==2)),"initial receipt");
    }
    expect(pipeline.observe({jobs[0].job.key,0},receipt(jobs[0],2,1,false)),"pending consumer");
    expect(pipeline.observe({jobs[1].job.key,1},receipt(jobs[1],3,1,true)),"open pin");
    const auto sample=input(pipeline.epochs(),3,102);
    const auto plan=pipeline.begin(sample,103);
    expect(plan.admission && plan.admission->slot==2 && plan.selection.kind==OutputKind::DelayedNr,
        "pending slot0 must not alter reservation2 or prior selection");
    // Legal completion after begin: physical slot0 is reusable; reservation stays2.
    expect(interop::reusable(receipt(jobs[0],2,2,false)),"newly completed lower slot");
    if(refused) {
        expect(pipeline.finish(raw(sample),nullptr),"refusal must consume reservation");
        await(plan.admission->returned);
        expect(pipeline.counters().dispatches==2,"refusal orphaned a dispatch");
    } else {
        const auto& a=*plan.admission;
        const interop::Ownership captured{a.job.key,interop::WorkPhase::Captured,3,0,0,3,1,2,false,false,false};
        const WorkBinding binding{{a.job.key,a.slot},captured,{},processor};
        expect(pipeline.finish(raw(sample),&binding),"exact reserved slot must dispatch");
        await(a.returned);jobs.push_back(a);
        expect(processor->lastTiming==std::array<int64_t,2>{},
            "work without a diagnostic clock must not inherit stamps from earlier jobs");
    }
    const auto& latched=pipeline.view(102)->selection;
    expect(latched.canonicalTick==plan.selection.canonicalTick && latched.kind==plan.selection.kind &&
        latched.status==plan.selection.status && latched.reason==plan.selection.reason &&
        latched.frame==plan.selection.frame && latched.job==plan.selection.job &&
        latched.resetTemporal==plan.selection.resetTemporal,"finish changed readiness latch");
    for(const auto& a:jobs) if(pipeline.state(a.job.key))
        expect(pipeline.observe({a.job.key,a.slot},receipt(a,3,3,false)),"final receipt");
    const auto exited=pipeline.worker_exit();pipeline.request_stop();await(exited);
    expect(pipeline.try_close(),"normal scheduler close");
}
void cold_gate() {
    auto processor=std::make_shared<Copy>();
    FramePipeline pipeline({41,1,1,1},processor,true);
    auto dispatch=[&](uint64_t seq,uint64_t tick,bool warmup) -> Admission {
        const auto sample=input(pipeline.epochs(),seq,tick);
        const auto plan=pipeline.begin(sample,tick+1);
        expect(plan.admission && plan.warmup==warmup && plan.admission->job.input==sample,
            "released admission must use the latest exact capture");
        const auto& a=*plan.admission;
        const auto producer=a.job.key.submissionSerial;
        const interop::Ownership captured{a.job.key,interop::WorkPhase::Captured,producer,0,0,0,0,0,false,false,false};
        const WorkBinding binding{{a.job.key,a.slot},captured,{},processor};
        expect(pipeline.finish(raw(sample),&binding),"cold dispatch");
        await(a.returned);
        return a;
    };
    const auto epoch=pipeline.epochs();
    auto skip=[&](uint64_t seq,uint64_t tick) {
        const auto sample=input(pipeline.epochs(),seq,tick);
        const auto plan=pipeline.begin(sample,tick+1);
        expect(!plan.admission && plan.admissionReason==Reason::ModelWarmup && plan.warmup &&
            plan.selection.kind==OutputKind::DelayedRaw && plan.selection.status==Status::Priming &&
            plan.selection.reason==Reason::ModelWarmup && !plan.selection.job && !plan.selection.resetTemporal,
            "an unresolved cold owner admits no stale follower and reports raw warmup");
        expect(pipeline.finish(raw(sample)),"cold raw-only capture");
        const auto* shown=pipeline.view(tick);
        expect(shown && shown->raw && shown->raw->input.key.ingressSequence==seq-1 &&
            *std::static_pointer_cast<const uint64_t>(shown->image)==seq-1 && pipeline.epochs().historyEpoch==epoch.historyEpoch,
            "warmup uses the real previous raw capture without resetting history");
    };
    auto finish=[&](const Admission& a,bool evaluated) {
        const auto serial=a.job.key.submissionSerial;
        const interop::Ownership done{a.job.key,evaluated?interop::WorkPhase::Submitted:interop::WorkPhase::Discarded,
            serial,evaluated?serial:0,0,serial,evaluated?serial:0,0,false,false,false};
        expect(pipeline.observe({a.job.key,a.slot},done),"exact physical cold receipt");
        expect(pipeline.finish_warmup(a.job.key,evaluated),"exact model cold receipt");
        expect(!pipeline.finish_warmup(a.job.key,true),"a cold receipt may be consumed only once");
    };
    const auto pass1=dispatch(1,100,true);
    const auto& key=pass1.job.key;
    expect(!pipeline.finish_warmup({key.frame,key.historyEpoch,key.resourceGeneration,key.submissionSerial+1},true),
        "foreign serial cannot advance startup");
    skip(2,101);skip(3,102);
    expect(pipeline.counters().dispatches==1,"queued followers survived the cold gate");
    finish(pass1,false);
    const auto pass2=dispatch(4,103,true);
    skip(5,104);
    finish(pass2,false);
    const auto evaluation=dispatch(6,105,true);
    skip(7,106);skip(8,107);
    finish(evaluation,true);
    const auto steady1=dispatch(9,108,false);
    const auto* shown=pipeline.view(108);
    expect(shown && shown->selection.kind==OutputKind::DelayedRaw && shown->selection.reason==Reason::ModelWarmup &&
        shown->raw && shown->raw->input.key.ingressSequence==8,"late first evaluation must not replace a newer raw frame");
    const auto steady2=dispatch(10,109,false); // No model receipt for 9 yet: steady admission permits overlap.
    expect(pipeline.counters().dispatches==5,"steady dispatch incorrectly requires an outstanding model receipt");
    expect(pipeline.reset(ResetCause::ExtentChanged),"new physical generation");
    const auto nextGeneration=dispatch(11,110,true);
    expect(!pipeline.finish_warmup(evaluation.job.key,true),"a retired generation cannot unlock current warmup");
    finish(nextGeneration,false);
    for(const auto* steady:{&steady1,&steady2}) {
        const auto serial=steady->job.key.submissionSerial;
        const interop::Ownership done{steady->job.key,interop::WorkPhase::Discarded,serial,0,0,serial,0,0,false,false,false};
        expect(pipeline.observe({steady->job.key,steady->slot},done),"retired steady ownership drains normally");
    }
    const auto exited=pipeline.worker_exit();pipeline.request_stop();await(exited);
    expect(pipeline.try_close(),"cold and ordinary slots drain on close");
}
}
int main() {
    try { run(false);run(true);cold_gate();std::puts("PASS slot_reservation exact2, raw-only refusal, and cold gate; CPU not physical Bridge proof");return 0; }
    catch(const std::exception& e) { std::fprintf(stderr,"FAIL slot_reservation %s\n",e.what());return 1; }
}
