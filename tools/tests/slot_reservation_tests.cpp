// SPDX-License-Identifier: GPL-3.0-only
#include "frame_pipeline.h"
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
struct Copy final:Processor {
    Processed process(const Work& w,std::stop_token) override { return {w.job,true,w.raw->image}; }
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
        const WorkBinding binding{{a.job.key,a.slot},captured,{},processor};
        expect(pipeline.finish(raw(sample),&binding),"initial dispatch");
        await(a.returned);jobs.push_back(a);
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
}
int main() {
    try { run(false);run(true);std::puts("PASS slot_reservation exact2 and raw-only refusal; CPU not physical Bridge proof");return 0; }
    catch(const std::exception& e) { std::fprintf(stderr,"FAIL slot_reservation %s\n",e.what());return 1; }
}
