// SPDX-License-Identifier: GPL-3.0-only
#include "frame_pipeline.h"
#include "nr_model.h"
#include "../host/nr_instance_json.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <vector>
using namespace nr;
using namespace nr::pipeline;
namespace {
void expect(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
void await(const std::shared_future<void>& signal) {
    expect(signal.valid() && signal.wait_for(std::chrono::seconds(5))==std::future_status::ready,
        "bounded exact event timeout");
    signal.get();
}
struct Dispatch final:Processor {
    const uint64_t instance,revision;
    std::atomic<uint64_t> evaluations{0};
    std::promise<void> enteredPromise,releasePromise;
    const std::shared_future<void> entered=enteredPromise.get_future().share();
    const std::shared_future<void> released=releasePromise.get_future().share();
    const bool gateFirst;
    Dispatch(uint64_t id,uint64_t settings,bool gate=false):instance(id),revision(settings),gateFirst(gate) {}
    Processed process(const Work& work,std::stop_token) override {
        expect(work.job.key.frame.instanceId==instance && work.raw->input==work.job.input &&
            work.job.input.settings.revision.value==revision,"dispatch crossed instance/settings/raw binding");
        const auto count=evaluations.fetch_add(1)+1;
        if(gateFirst && count==1) { enteredPromise.set_value();await(released); }
        return {work.job,true,work.raw->image};
    }
};
IngressSample input(FramePipeline& pipeline,uint64_t sequence,uint64_t tick,uint64_t revision) {
    const auto e=pipeline.epochs();
    return {{e.instanceId,e.contentEpoch,sequence},tick,tick-1,e.instanceId+100,900,e.resourceGeneration,SourceValidity::Valid,
        {{64,32},{0,0,64,32},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},
        {{revision},true,"C:/readonly/not-loaded.dll",{revision==11?.25f:.75f},{0,0,1,1,0,-1,true}}};
}
std::shared_ptr<const RawFrame> raw(const IngressSample& sample) {
    // Disjoint content identities even when both instances use the same tick/sequence.
    return std::make_shared<const RawFrame>(RawFrame{sample,
        std::make_shared<const uint64_t>(sample.key.instanceId*1000+sample.key.ingressSequence)});
}
interop::Ownership receipt(const Admission& a,bool open=false) {
    const auto serial=a.job.key.submissionSerial;
    return {a.job.key,interop::WorkPhase::Submitted,serial,serial,serial,serial,serial,serial,open,false,false};
}
Admission dispatch(FramePipeline& p,const IngressSample& sample) {
    const auto plan=p.begin(sample,sample.canonicalTick+1);
    expect(plan.action==TickAction::CaptureOnce && plan.admission.has_value(),"independent capture admission");
    const auto a=*plan.admission;
    const auto repeated=p.begin(sample,sample.canonicalTick+1);
    expect(repeated.action==TickAction::AwaitLatch && repeated.admission &&
        repeated.admission->job==a.job && repeated.admission->slot==a.slot,"pre-finish reference duplicated reservation");
    const auto image=raw(sample);
    const interop::Ownership captured{a.job.key,interop::WorkPhase::Captured,
        a.job.key.submissionSerial,0,0,0,0,0,false,false,false};
    const WorkBinding binding{{a.job.key,a.slot},captured,{},image->image?std::make_shared<int>(1):nullptr};
    // Admission.returned is already subscribed before finish enqueues actual work.
    expect(p.finish(image,&binding),"exact instance dispatch");
    expect(!p.finish(image,&binding),"repeat finish dispatched twice");
    const auto* view=p.view(sample.canonicalTick);
    expect(view!=nullptr,"latched view missing");
    for(unsigned n=0;n<4;++n) {
        expect(p.view(sample.canonicalTick)==view,"repeated reference changed publication");
        expect(p.begin(sample,sample.canonicalTick+1).action==TickAction::ReuseLatch,
            "latched reference captured again");
    }
    return a;
}
void complete(FramePipeline& p,const Admission& a,bool open=false) {
    await(a.returned);
    expect(p.observe({a.job.key,a.slot},receipt(a,open)),"exact production ownership receipt rejected");
}
void close(FramePipeline& a,FramePipeline& b) {
    const auto exitA=a.worker_exit(),exitB=b.worker_exit();
    a.request_stop();b.request_stop();await(exitA);await(exitB);
    expect(a.try_close() && b.try_close(),"normal independent pipeline closure");
    std::puts("CLOSURE worker_exit_A=true worker_exit_B=true try_close_A=true try_close_B=true");
}
void alternating(bool sameLocalEpochs=false) {
    auto pa=std::make_shared<Dispatch>(41,11),pb=std::make_shared<Dispatch>(87,29);
    FramePipeline a({41,3,5,7},pa),b({87,sameLocalEpochs?3u:13u,sameLocalEpochs?5u:17u,sameLocalEpochs?7u:19u},pb);
    for(uint64_t seq=1;seq<=6;++seq) {
        const auto sa=input(a,seq,100+seq,11),sb=input(b,seq,100+seq,29);
        const auto ja=seq%2?dispatch(a,sa):dispatch(b,sb);
        const auto jb=seq%2?dispatch(b,sb):dispatch(a,sa);
        complete(seq%2?a:b,ja);complete(seq%2?b:a,jb);
        if(sameLocalEpochs) {
            const auto& ownA=seq%2?ja:jb;
            const auto& ownB=seq%2?jb:ja;
            expect(!a.observe({ownB.job.key,ownB.slot},receipt(ownB)) &&
                !b.observe({ownA.job.key,ownA.slot},receipt(ownA)),
                "same-valued local epochs admitted foreign instance receipt");
        }
        const auto* va=a.view(sa.canonicalTick);const auto* vb=b.view(sb.canonicalTick);
        expect(va && vb && va->image!=vb->image && va->raw && vb->raw,"disjoint selected content owners");
        expect(va->raw->input.key.instanceId==41 && vb->raw->input.key.instanceId==87 &&
            va->raw->input.settings==sa.settings && vb->raw->input.settings==sb.settings,
            "selection crossed settings or instance");
        const auto expected=seq==1?OutputKind::PrimingRaw:OutputKind::DelayedNr;
        expect(va->selection.kind==expected && vb->selection.kind==expected,"canonical prior selection not NR");
        expect(va->raw->input.key.ingressSequence==(seq==1?1:seq-1) &&
            vb->raw->input.key.ingressSequence==(seq==1?1:seq-1),"selected wrong canonical predecessor");
        if(seq>1)expect(va->selection.job && vb->selection.job &&
            va->selection.job->historyEpoch==5 && vb->selection.job->historyEpoch==(sameLocalEpochs?5u:17u) &&
            va->selection.job->resourceGeneration==7 && vb->selection.job->resourceGeneration==(sameLocalEpochs?7u:19u),
            "selected wrong instance epochs");
    }
    expect(pa->evaluations==6 && pb->evaluations==6 && a.counters().dispatches==6 &&
        b.counters().dispatches==6 && a.counters().captures==6 && b.counters().captures==6 &&
        a.counters().selections==6 && b.counters().selections==6,"more than one evaluation/capture/selection per tick");
    expect(!sameLocalEpochs || a.epochs().contentEpoch==b.epochs().contentEpoch,"same-valued local epochs rejected");
    close(a,b);std::puts("PASS interleaved_instances ticks=6 evaluations_A=6 evaluations_B=6 repeated_views=48");
}
void late() {
    auto pa=std::make_shared<Dispatch>(41,11,true),pb=std::make_shared<Dispatch>(87,29);
    FramePipeline a({41,3,5,7},pa),b({87,13,17,19},pb);
    const auto firstA=dispatch(a,input(a,1,201,11));
    await(pa->entered); // Exact worker-entry signal; A cannot return until explicit release.
    expect(firstA.returned.wait_for(std::chrono::seconds(0))==std::future_status::timeout &&
        a.state(firstA.job.key) && !a.state(firstA.job.key)->dispatchReturned,
        "processor readiness must not assert exact scheduler dispatch return");
    const auto firstB=dispatch(b,input(b,1,201,29));complete(b,firstB);
    const auto secondB=dispatch(b,input(b,2,202,29));complete(b,secondB);
    const auto secondA=dispatch(a,input(a,2,202,11));
    const auto* beforeB=b.view(202);
    const auto bImage=beforeB->image;
    expect(beforeB->selection.kind==OutputKind::DelayedNr,"B blocked behind A");
    expect(a.view(202)->selection.kind==OutputKind::DelayedRaw,"unreturned A incorrectly published");
    expect(a.state(firstA.job.key)->publication==PublicationState::Expired,"late A not expired at cutoff");
    pa->releasePromise.set_value();complete(a,firstA);complete(a,secondA);
    expect(!b.observe({firstA.job.key,firstA.slot},receipt(firstA)),"late foreign A receipt accepted into B");
    expect(!a.observe({firstB.job.key,firstB.slot},receipt(firstB)),"foreign B receipt accepted into A");
    expect(b.view(202)==beforeB && beforeB->image==bImage &&
        beforeB->selection.job==firstB.job.key,"late A changed B publication");
    expect(a.view(202)->selection.kind==OutputKind::DelayedRaw &&
        a.state(firstA.job.key)->publication==PublicationState::Expired,"late A resurrected expired output");
    const auto thirdA=dispatch(a,input(a,3,203,11));complete(a,thirdA);
    const auto thirdB=dispatch(b,input(b,3,203,29));complete(b,thirdB);
    expect(a.view(203)->selection.kind==OutputKind::DelayedNr &&
        b.view(203)->selection.kind==OutputKind::DelayedNr,"independent post-late recovery failed");
    close(a,b);std::puts("PASS late_A expired=true foreign_receipts_rejected=true B_unaffected=true");
}
void bounded() {
    auto pa=std::make_shared<Dispatch>(41,11),pb=std::make_shared<Dispatch>(87,29);
    FramePipeline a({41,3,5,7},pa),b({87,13,17,19},pb);
    std::vector<Admission> aa,bb;
    for(uint64_t seq=1;seq<=3;++seq) {
        aa.push_back(dispatch(a,input(a,seq,300+seq,11)));
        bb.push_back(dispatch(b,input(b,seq,300+seq,29)));
        complete(a,aa.back(),true);complete(b,bb.back(),true);
        expect(aa.back().slot==seq-1 && bb.back().slot==seq-1,"three slots not independently available");
    }
    for(auto* p:{&a,&b}) {
        const auto sample=input(*p,4,304,p==&a?11:29);
        const auto plan=p->begin(sample,305);
        expect(!plan.admission && plan.admissionReason==Reason::SlotsFull,"allocated slot four or rejected earlier");
        expect(p->finish(raw(sample)),"bounded raw finish");
    }
    for(const auto& j:aa)expect(a.observe({j.job.key,j.slot},receipt(j)),"A consumer close receipt");
    for(const auto& j:bb)expect(b.observe({j.job.key,j.slot},receipt(j)),"B consumer close receipt");
    const auto recoveredA=dispatch(a,input(a,5,305,11)),recoveredB=dispatch(b,input(b,5,305,29));
    complete(a,recoveredA);complete(b,recoveredB);
    for(auto* p:{&a,&b}) {
        for(unsigned n=0;n<2;++n) {
            expect(p->hold_generation(p->epochs().resourceGeneration,std::make_shared<int>(1)),"exact generation hold");
            expect(p->reset(ResetCause::SettingsChanged),"independent retirement admission");
        }
        expect(p->retired_generations()==2,"retired capacity not exactly two");
        const auto before=p->epochs();
        expect(!p->reset(ResetCause::SettingsChanged) && p->epochs()==before,
            "allocated retired generation three or mutated blocked epochs");
    }
    expect(!a.complete_generation(19) && !b.complete_generation(7),"foreign generation receipt accepted");
    expect(a.complete_generation(7) && a.complete_generation(8),"A exact generation completion");
    a.collect();
    expect(a.retired_generations()==0 && b.retired_generations()==2,"A retirement completion collected B");
    expect(a.reset(ResetCause::SettingsChanged),"A remained arbitrarily rejected after capacity recovered");
    expect(b.complete_generation(19) && b.complete_generation(20),"B exact generation completion");
    b.collect();expect(b.retired_generations()==0,"B completed generations retained");
    close(a,b);std::puts("PASS bounded_instances slots_each=3 retired_each=2 independent_recovery=true");
}
void warmup_gap() {
    auto pa=std::make_shared<Dispatch>(41,11),pb=std::make_shared<Dispatch>(87,29);
    FramePipeline a({41,3,5,7},pa),b({87,13,17,19},pb);
    const auto firstA=dispatch(a,input(a,1,401,11)),firstB=dispatch(b,input(b,1,401,29));
    complete(a,firstA);complete(b,firstB);
    const auto gap=input(a,2,404,11);
    const auto plan=a.begin(gap,405);
    expect(plan.selection.kind==OutputKind::CurrentSource && plan.selection.reason==Reason::KeyMismatch &&
        !plan.admission,"genuine warmup gap must bypass without dispatch");
    expect(a.finish({}),"capture-free gap latch");
    const auto* bypass=a.view(404);
    expect(bypass && !bypass->raw && !bypass->selection.frame && !bypass->selection.job,
        "gap bypass retained stale raw/model binding");
    expect(a.begin(gap,405).action==TickAction::ReuseLatch && a.view(404)==bypass &&
        pa->evaluations==1,"repeated bypass fabricated a job or changed observation");
    const auto nextB=dispatch(b,input(b,2,402,29));complete(b,nextB);
    expect(b.view(402)->selection.kind==OutputKind::DelayedNr,"A warmup gap reset B");
    const auto priming=dispatch(a,input(a,3,405,11));complete(a,priming);
    expect(a.view(405)->selection.kind==OutputKind::PrimingRaw,"gap recovery must truthfully re-prime");
    const auto ordinary=dispatch(a,input(a,4,406,11));complete(a,ordinary);
    expect(a.view(406)->selection.kind==OutputKind::DelayedNr &&
        a.view(406)->selection.job==priming.job.key,"ordinary successor lost exact recovered predecessor");
    close(a,b);std::puts("PASS warmup_gap capture_free=true stale_binding_rejected=true B_unaffected=true ordinary_recovery=true");
}
struct Availability final:model::ObservationSink {
    bool armed=false;
    std::optional<JobKey> key;
    model::Observation* reserve(const IngressSample&,const TickPlan& plan) noexcept override {
        if(armed && plan.admission) { key.emplace(plan.admission->job.key);armed=false; }
        return nullptr;
    }
    void capture(model::Observation*,ID3D11Texture2D*,ID3D11Texture2D*,ID3D11Texture2D*) noexcept override {}
    bool publication_available(const JobKey& candidate) const noexcept override { return !key || candidate!=*key; }
};
struct SettlingDispatch final:Processor {
    const uint64_t instance,revision;
    std::atomic<bool> settled{true};
    std::array<bool,6> evaluated{};
    std::array<unsigned,6> calls{};
    std::promise<void> heldPromise,physicalPromise;
    const std::shared_future<void> held=heldPromise.get_future().share();
    const std::shared_future<void> physical=physicalPromise.get_future().share();
    SettlingDispatch(uint64_t id,uint64_t settings):instance(id),revision(settings) {}
    Processed process(const Work& work,std::stop_token) override {
        const auto seq=work.job.input.key.ingressSequence;
        expect(seq>=1 && seq<=6 && work.job.key.frame.instanceId==instance &&
            work.job.input==work.raw->input && work.job.input.settings.revision.value==revision,
            "settling dispatch exact immutable owner");
        ++calls[seq-1];
        if(!settled.load(std::memory_order_acquire))return {work.job,false,{}};
        evaluated[seq-1]=true;
        if(instance==41 && seq==3) { settled.store(false,std::memory_order_release);heldPromise.set_value(); }
        return {work.job,true,work.raw->image}; // Return alone is not physical settlement.
    }
    void complete_physical() {
        expect(!settled.exchange(true,std::memory_order_acq_rel),"exact physical work already completed");
        physicalPromise.set_value();
    }
};
Admission publication_dispatch(FramePipeline& p,const IngressSample& sample,Availability& sink) {
    const auto plan=p.begin(sample,sample.canonicalTick+1);
    expect(plan.admission && plan.action==TickAction::CaptureOnce,"publication regression admission");
    const auto job=*plan.admission;
    sink.reserve(sample,plan); // Full key and exact return signal exist before finish triggers work.
    const interop::Ownership captured{job.job.key,interop::WorkPhase::Captured,
        job.job.key.submissionSerial,0,0,0,0,0,false,false,false};
    const WorkBinding binding{{job.job.key,job.slot},captured,{},std::make_shared<int>(1)};
    expect(p.finish(raw(sample),&binding),"publication regression dispatch");
    return job;
}
void publication_late(bool oldPhysical=false) {
    auto pa=std::make_shared<SettlingDispatch>(41,11),pb=std::make_shared<SettlingDispatch>(87,29);
    FramePipeline a({41,3,5,7},pa),b({87,13,17,19},pb);
    Availability availableA,availableB;
    std::optional<Admission> lateA,priorA,priorB;
    unsigned views=0;
    for(uint64_t seq=1;seq<=6;++seq) {
        const auto sa=input(a,seq,500+seq,11),sb=input(b,seq,500+seq,29);
        if(seq==3 && !oldPhysical)availableA.armed=true;
        // Alternate dispatch order just as the host does, before either exact return wait.
        const auto ja=seq%2?publication_dispatch(a,sa,availableA):publication_dispatch(b,sb,availableB);
        const auto jb=seq%2?publication_dispatch(b,sb,availableB):publication_dispatch(a,sa,availableA);
        const auto& ownA=seq%2?ja:jb;const auto& ownB=seq%2?jb:ja;
        await(ownA.returned);await(ownB.returned);
        if(seq==3) {
            lateA.emplace(ownA);await(pa->held);
            if(!oldPhysical) { pa->complete_physical();await(pa->physical); }
        }
        const auto observedA=seq==3 && oldPhysical?
            interop::Ownership{ownA.job.key,interop::WorkPhase::Submitted,
                ownA.job.key.submissionSerial,ownA.job.key.submissionSerial,0,
                ownA.job.key.submissionSerial,0,0,false,false,false}:
            (pa->evaluated[seq-1]?receipt(ownA):
                interop::Ownership{ownA.job.key,interop::WorkPhase::Discarded,
                    ownA.job.key.submissionSerial,0,0,ownA.job.key.submissionSerial,0,0,false,false,false});
        expect(model::observe_publication(a,{ownA.job.key,ownA.slot},observedA,&availableA) &&
            model::observe_publication(b,{ownB.job.key,ownB.slot},receipt(ownB),&availableB),
            "shared QA completion availability receipt");
        const auto* va=a.view(sa.canonicalTick);const auto* vb=b.view(sb.canonicalTick);
        expect(va && vb && va->raw && vb->raw && va->image!=vb->image,"publication disjoint raw/image owners");
        for(unsigned view=0;view<3;++view) {
            expect(a.begin(sa,sa.canonicalTick+1).action==TickAction::ReuseLatch &&
                b.begin(sb,sb.canonicalTick+1).action==TickAction::ReuseLatch &&
                a.view(sa.canonicalTick)==va && b.view(sb.canonicalTick)==vb,
                "publication repeated view mutated latch or dispatch");views+=2;
        }
        expect(vb->selection.kind==(seq==1?OutputKind::PrimingRaw:OutputKind::DelayedNr) &&
            (seq==1 || vb->selection.job==priorB->job.key),"B changed behind A availability");
        expect(va->raw->input==((seq==1)?sa:priorA->job.input) &&
            vb->raw->input==((seq==1)?sb:priorB->job.input),"publication exact predecessor/settings/key");
        if(!oldPhysical)expect(va->selection.kind==(seq==1?OutputKind::PrimingRaw:
            (seq==4?OutputKind::DelayedRaw:OutputKind::DelayedNr)) &&
            (seq==1 || seq==4 || va->selection.job==priorA->job.key),"A publication cutoff selection");
        if(seq==4) {
            expect(va->selection.kind==OutputKind::DelayedRaw && !va->selection.job,"A3 availability cutoff not raw");
            const auto aImage=va->image,bImage=vb->image;
            if(oldPhysical) { pa->complete_physical();await(pa->physical); }
            else availableA.key.reset(); // Release only AFTER BOTH cutoffs and all three repeated views.
            const auto lateState=a.state(lateA->job.key);
            expect(!lateState || lateState->publication==PublicationState::Expired,"late A2 not expired");
            if(lateState)expect(model::observe_publication(a,{lateA->job.key,lateA->slot},receipt(*lateA),&availableA),
                "expired A2 physical ownership collection");
            expect(a.begin(sa,sa.canonicalTick+1).action==TickAction::ReuseLatch &&
                b.begin(sb,sb.canonicalTick+1).action==TickAction::ReuseLatch &&
                a.view(sa.canonicalTick)==va && va->image==aImage && va->selection.kind==OutputKind::DelayedRaw &&
                b.view(sb.canonicalTick)==vb && vb->image==bImage,"released A2 resurrected current latch or changed B");
            if(oldPhysical)expect(!pa->evaluated[3],"old physical hold did not expose fail-closed current dispatch");
        }
        expect(a.retired_generations()<=2 && b.retired_generations()<=2 && ownA.slot<3 && ownB.slot<3,
            "publication bounded ownership");
        priorA.emplace(ownA);priorB.emplace(ownB);
    }
    bool all=true;unsigned evaluationsA=0,evaluationsB=0;
    for(size_t i=0;i<6;++i) {
        all=all && pa->evaluated[i] && pb->evaluated[i] && pa->calls[i]==1 && pb->calls[i]==1;
        evaluationsA+=pa->evaluated[i]?1u:0u;evaluationsB+=pb->evaluated[i]?1u:0u;
    }
    expect(views==36 && a.counters().dispatches==6 && b.counters().dispatches==6 &&
        a.counters().captures==6 && b.counters().captures==6 &&
        a.counters().selections==6 && b.counters().selections==6,"strict publication 36 views/12 unique dispatches");
    close(a,b);
    std::printf("PUBLICATION_REGRESSION mode=%s views=36 dispatches=12 evaluations_A=%u evaluations_B=%u "
        "A3_DelayedRaw=true B_unaffected=true normal_close=true\n",
        oldPhysical?"old_physical_RED":"publication_only_GREEN",evaluationsA,evaluationsB);
    expect(all,"all six current jobs must evaluate per instance; old physical hold violates settlement");
    std::puts("PASS publication_only late_A2_expired=true no_resurrection=true");
}
void availability_close() {
    for(unsigned mode=0;mode<3;++mode) {
        auto pa=std::make_shared<Dispatch>(41,11),pb=std::make_shared<Dispatch>(87,29);
        FramePipeline a({41,3,5,7},pa),b({87,13,17,19},pb);
        Availability sink;sink.armed=true;
        const auto job=publication_dispatch(a,input(a,1,601,11),sink);await(job.returned);
        expect(model::observe_publication(a,{job.job.key,job.slot},receipt(job),&sink) &&
            a.state(job.job.key)->physical.workerDone==0,"availability did not withhold exact pending completion");
        if(mode==0)a.request_stop();
        else if(mode==1)expect(a.reset(ResetCause::SettingsChanged),"availability retirement reset");
        else expect(a.cancel(job.job.key),"availability terminal cancel");
        expect(model::observe_publication(a,{job.job.key,job.slot},receipt(job),&sink,mode==0) &&
            a.state(job.job.key)->physical.workerDone==receipt(job).workerDone &&
            sink.key==job.job.key,"stop/retirement/terminal availability falsely retained complete owner");
        close(a,b);
    }
    std::puts("PASS availability_bypass stop=true retirement=true terminal=true no_gate_release=true");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::strcmp(argv[1],"--old-physical-gate")==0) { publication_late(true);return 0; }
        std::ostringstream encoded;
        nr::qa::write_instance_json_string(encoded,"\"\\\n\r\t\b\f\001");
        expect(encoded.str()=="\"\\\"\\\\\\u000a\\u000d\\u0009\\u0008\\u000c\\u0001\"","instance JSON quote/backslash/control escaping");
        std::ostringstream absent;nr::qa::write_instance_json_string(absent,nullptr);
        expect(absent.str()=="\"\"","missing instance diagnostic string");
        std::puts("PASS instance_json quotes=true backslash=true controls=true missing=true");
        alternating();alternating(true);late();bounded();warmup_gap();publication_late();availability_close();
        std::puts("CPU_ALL_PASS cases=8 native=false GPU=false OBS=false SDK=false");return 0;
    }
    catch(const std::exception& e) { std::fprintf(stderr,"FAIL instance_contract %s\n",e.what());return 1; }
}
