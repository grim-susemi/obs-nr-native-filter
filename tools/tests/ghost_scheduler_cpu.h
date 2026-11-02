// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "frame_pipeline.h"
#include <chrono>

namespace ghost_scheduler_cpu {
using namespace nr;
using namespace nr::ghost;
using namespace nr::pipeline;

// Exercise the owned production scheduler with CPU-owned pixels. No GPU object,
// vendor model, compositor substitute, or encoder is constructed by this test.
struct Observation {
    const std::shared_ptr<const RawFrame> raw, reference;
    const JobKey job;
};
class Processor final : public nr::pipeline::Processor {
    const Fixture& fixture_;
    std::shared_ptr<const RawFrame> previous_;
public:
    explicit Processor(const Fixture& fixture) : fixture_(fixture) {}
    Processed process(const Work& work, std::stop_token) override
    {
        const auto tick=static_cast<unsigned>(work.job.input.key.ingressSequence-1);
        const auto reference=!previous_||fixture_.schedule[tick].cut?work.raw:previous_;
        auto observation=std::static_pointer_cast<std::optional<Observation>>(work.binding.processorLease);
        observation->emplace(Observation{work.raw,reference,work.job.key});
        previous_=work.raw;
        return {work.job,true,work.raw->image};
    }
};
inline void run()
{
    unsigned rows=0,rejected=0;
    for(auto seed:{0xA17u,0xB29u})
    for(auto color:{FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020})
    for(auto rate:{60u,60000u}) {
        const auto fixture=nr::ghost::fixture(seed,color);
        const bool sdr=color==FixtureColor::Sdr||color==FixtureColor::DarkSdr;
        const FrameDescriptor descriptor{{192,96},{0,0,192,96},
            sdr?PixelFormat::Rgba8:PixelFormat::Rgba16Float,
            sdr?ColorSpace::Rec709Srgb:color==FixtureColor::Hdr709?ColorSpace::ScRgbLinear:ColorSpace::Rec2020Linear,
            AlphaMode::Opaque,static_cast<float>(fixture.referenceWhiteNits)};
        auto processor=std::make_shared<Processor>(fixture);
        FramePipeline pipeline({41,1,1,1},processor);
        std::shared_ptr<std::optional<Observation>> previous;
        for(unsigned tick=0;tick<256;++tick) {
            const auto epochs=pipeline.epochs();
            const IngressSample input{{41,epochs.contentEpoch,tick+1},100+tick,99+tick,301,901,
                epochs.resourceGeneration,SourceValidity::Valid,descriptor,
                {{1},true,"C:/owned/CPU-not-loaded.dll",{.75f},{0,0,1,1,0,-1,true}}};
            const auto plan=pipeline.begin(input,input.canonicalTick+1);
            expect(plan.action==TickAction::CaptureOnce&&plan.admission.has_value(),"production scheduler admission");
            const auto observation=std::make_shared<std::optional<Observation>>();
            const auto raw=std::make_shared<const RawFrame>(RawFrame{input,
                std::make_shared<const Image>(clean_image(fixture,tick))});
            const auto& admitted=*plan.admission;
            const interop::Ownership captured{admitted.job.key,interop::WorkPhase::Captured,1,0,0,0,0,0,false,false,false};
            const WorkBinding binding{{admitted.job.key,admitted.slot},captured,{},observation};
            // admitted.returned is subscribed before finish triggers the worker.
            expect(pipeline.finish(raw,&binding),"production dispatch exact immutable raw");
            const auto* selected=pipeline.view(input.canonicalTick);
            expect(selected&&selected->raw&&selected->image,"production publication pixels");
            const unsigned sourceTick=tick?tick-1:tick;
            expect(selected->selection.kind==(tick?OutputKind::DelayedNr:OutputKind::PrimingRaw),
                "production n-1 selection");
            expect(selected->selection.frame==selected->raw->input.key&&
                selected->raw->input.key.ingressSequence==sourceTick+1,"production ingress progression during holds");
            expect(decode_content(*std::static_pointer_cast<const Image>(selected->image))==
                fixture.schedule[sourceTick].contentId,"production image-decoded content");
            if(tick) {
                expect(previous&&previous->has_value(),"completed worker observation missing");
                const auto& completed=previous->value();
                expect(selected->selection.job==completed.job,"selected exact completed job");
                std::vector<Capture> captures;
                for(auto stage:{Stage::Raw,Stage::ModelInput,Stage::GuideCurrent,Stage::GuideReference,
                    Stage::Unprotected,Stage::Completed,Stage::Selected,Stage::Preencode,Stage::Decoded}) {
                    const auto source=stage==Stage::GuideReference?completed.reference:completed.raw;
                    const auto pixels=stage==Stage::Selected||stage==Stage::Preencode||stage==Stage::Decoded?
                        selected->image:source->image;
                    captures.push_back({stage,source->input.key,completed.job,descriptor,1,
                        *std::static_pointer_cast<const Image>(pixels),1,"CPU_PRODUCTION_SCHEDULER"});
                }
                const auto valid=[&](std::span<const Capture> trace) {
                    return lineage(fixture,tick,selected->selection.kind,trace,0,
                        completed.raw->input.key,completed.reference->input.key,&descriptor);
                };
                expect(valid(captures),"production captured pairs failed lineage");
                if(tick==41) {
                    auto stale=captures;
                    stale[3].pixels=clean_image(fixture,38);
                    expect(!valid(stale),"production stale guide falsely green");++rejected;
                    auto relabeled=captures;
                    const FrameKey staleKey{41,epochs.contentEpoch,39};
                    std::destroy_at(&relabeled[3]);
                    std::construct_at(&relabeled[3],Capture{Stage::GuideReference,staleKey,completed.job,descriptor,1,
                        clean_image(fixture,38),1,"CPU_PRODUCTION_SCHEDULER"});
                    expect(!lineage(fixture,tick,selected->selection.kind,relabeled,0,
                        completed.raw->input.key,staleKey,&descriptor),
                        "production relabeled stale guide pair falsely green");++rejected;
                    auto reordered=captures;
                    reordered[5].job.emplace(JobKey{{41,epochs.contentEpoch,39},epochs.historyEpoch,epochs.resourceGeneration,39});
                    expect(!valid(reordered),"production reordered completion falsely green");++rejected;
                    auto frozen=captures;
                    frozen[6].pixels=clean_image(fixture,20);
                    expect(!valid(frozen),"production frozen pixels falsely green");++rejected;
                    auto stamp=captures;
                    stamp[6].pixels=clean_image(fixture,38);
                    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<192;++x)
                        stamp[6].pixels[y*192+x]=captures[6].pixels[y*192+x];
                    expect(!valid(stamp),"production metadata-only stamp falsely green");++rejected;
                    std::vector<Capture> dropped;
                    for(const auto& capture:captures)if(capture.stage!=Stage::Selected)dropped.push_back(capture);
                    expect(!valid(dropped),"production dropped selected sample falsely green");++rejected;
                    expect(!lineage(fixture,tick+1,selected->selection.kind,captures,0,
                        completed.raw->input.key,completed.reference->input.key,&descriptor),
                        "production extra delay falsely green");++rejected;
                }
            }
            // Extra views must reuse the owned latch rather than recapture pixels.
            expect(pipeline.begin(input,input.canonicalTick+1).action==TickAction::ReuseLatch,
                "production repeated view recaptured");
            expect(admitted.returned.wait_for(std::chrono::seconds(5))==std::future_status::ready,
                "production exact worker-return event timeout");
            admitted.returned.get();
            const interop::Ownership done{admitted.job.key,interop::WorkPhase::Submitted,1,1,0,1,1,0,false,false,false};
            expect(pipeline.observe(binding.ticket,done),"production CPU completion receipt");
            previous=observation;
            if(tick)expect(canonical_time(tick,rate,rate==60?1:1001)>
                canonical_time(tick-1,rate,rate==60?1:1001),"production rational timestamp mapping");
            ++rows;
        }
        const auto exited=pipeline.worker_exit(); // Subscription before cooperative stop.
        pipeline.request_stop();
        expect(exited.wait_for(std::chrono::seconds(5))==std::future_status::ready,"production worker exit timeout");
        exited.get();
        expect(pipeline.try_close(),"production scheduler ownership not retired");
    }
    std::printf("CPU_PRODUCTION_SCHEDULER GREEN rows=%u episodes=16 rates=60/1,60000/1001 RED rejected=%u controls=stale-guide,relabeled-stale-guide,reordered-completion,frozen,metadata-only-stamp,dropped,extra-delay GPU=false compositor=false codec=false\n",rows,rejected);
}
} // namespace ghost_scheduler_cpu
