// SPDX-License-Identifier: GPL-3.0-only
#include "nr_core.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace nr;
namespace {
void expect(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int handles[12];
enum class Fault { None, WorkMissing, WorkExtent, ModelExtent, BExtent, CMissing, BCAlias, WorkAlias };
struct Spec { uint32_t passes=2,requested=2,scale=100;Fault fault=Fault::None; };
CoreInput fixture(Spec s={}) {
    const Extent native{16,8},work=work_extent({0,0,16,8},s.scale);
    const auto texture=[](int id,Extent size) {return CoreTexture{&handles[id],size,{0,0,size.width,size.height}};};
    const CoreTexture missing{nullptr,{0,0},{0,0,0,0}};
    const FrameKey fk{7,1,10};
    const FrameDescriptor d{native,{0,0,16,8},PixelFormat::Rgba16Float,ColorSpace::ScRgbLinear,AlphaMode::Straight,80};
    const Settings settings{{1},true,"fake",{.5f},core_default_tuning(),s.requested,s.scale};
    const JobContract j{{fk,1,1,1},{fk,10,9,1,1,1,SourceValidity::Valid,d,settings},11};
    const CoreResources r{texture(0,native),texture(1,native),texture(2,native),
        texture(3,s.fault==Fault::ModelExtent?Extent{1,1}:work),texture(4,native),texture(8,native),texture(9,native),
        reinterpret_cast<ID3D12DescriptorHeap*>(&handles[10])};
    const auto workProxy=s.fault==Fault::WorkMissing?missing:
        texture(s.fault==Fault::WorkAlias?1:5,s.fault==Fault::WorkExtent?native:work);
    const auto b=texture(6,s.fault==Fault::BExtent?Extent{1,1}:work);
    const auto c=s.fault==Fault::CMissing?missing:texture(s.fault==Fault::BCAlias?6:7,work);
    return {j,{7,1,1,1},r,{1,true,false},&handles[11],1,float(work.width)/16,float(work.height)/8,
        ColorSpace::ScRgbLinear,AlphaMode::Straight,s.passes,workProxy,b,c};
}
struct Probe {
    std::vector<int> calls;
    std::vector<uint32_t> indices,slots;
    uint32_t failEval=0,notReady=0,refuseClamp=0,recordedClamps=0;
    bool downsample=true,resolve=true;
    void* source=nullptr;
    void* answer=nullptr;
};
struct CodecProbe final : Codec {
    Probe& p;
    explicit CodecProbe(Probe& value):p(value){}
    bool encode(const CoreInput&,const CodecConstants& c) override {
        p.calls.push_back(1);
        expect(c.mode==0 && c.width==16 && c.height==8 && c.strength==.5f &&
            c.transferStrength==1 && c.colourStrength==1 && c.maxRatio==2,"encode constants changed");
        return true;
    }
    bool downsample(const CoreInput&,const CoreTexture& from,const CoreTexture& to) override {
        p.calls.push_back(4);expect(from.native==&handles[1] && to.native==&handles[5],"downsample pair");return p.downsample;
    }
    bool clamp(const CoreInput&,const CoreTexture& from,const CoreTexture& to,uint32_t slot) override {
        p.calls.push_back(30+int(slot));p.slots.push_back(slot);
        expect(from.native==&handles[slot?6:3] && to.native==&handles[7],"clamp A/B to C");
        if(p.refuseClamp==slot+1) return false;
        ++p.recordedClamps;return true;
    }
    bool resolve(const CoreInput&,const CodecConstants& c,const CoreTexture& source,const CoreTexture& answer) override {
        p.calls.push_back(5);p.source=source.native;p.answer=answer.native;
        expect(c.mode==1 && c.width==16 && c.height==8 && c.strength==.5f,"resolve constants changed");return p.resolve;
    }
};
struct ModelProbe final : ModelEvaluator {
    Probe& p;
    explicit ModelProbe(Probe& value):p(value){}
    EvaluationResult evaluate(const ModelInput& m) override {
        p.calls.push_back(20+int(m.pass));p.indices.push_back(m.pass);
        expect(m.color.native==&handles[m.pass?7:m.input.job.input.settings.workScale<100?5:1],"wrong pass input");
        expect(m.output.native==&handles[m.pass%2?6:3],"wrong A/B output");
        expect(m.depth.native==&handles[8] && m.motion.native==&handles[9],"guides changed between passes");
        if(m.input.job.input.settings.pass2Tuning && m.pass==1) {
            const auto& p2=*m.input.job.input.settings.pass2Tuning;
            expect(m.tuning.preset==p2.preset && m.tuning.style==p2.style &&
                m.tuning.intensity==p2.intensity && m.tuning.localTone==p2.localTone,"custom pass2 tuning");
        } else {
            expect(m.tuning.localTone==(m.pass?0:1),"per-pass local tone");
        }
        expect(m.input.mvScaleX==float(m.color.active.width)/16 && m.input.mvScaleY==float(m.color.active.height)/8,
            "motion scale changed");
        if(p.failEval==m.pass+1) return {EvaluationState::Error,0xbad};
        if(p.notReady==m.pass+1) return {EvaluationState::NotReady,0xabc};
        return {EvaluationState::Evaluated,0x100+m.pass};
    }
};
Core core(Probe& p) {return Core(7,std::make_unique<CodecProbe>(p),std::make_unique<ModelProbe>(p));}
void chains() {
    {
        Probe p;auto c=core(p);
        auto in=fixture({2,2,100});
        const ModelTuning p2{3,2,1.5f,0.75f,0.25f,0.5f,false};
        const Settings custom{{2},true,"fake",{.5f},core_default_tuning(),2,100,p2};
        const JobContract jc{{in.job.key.frame,1,1,1},{in.job.key.frame,10,9,1,1,1,SourceValidity::Valid,in.job.input.descriptor,custom},11};
        const CoreInput customIn{jc,in.epochs,in.resources,in.history,in.commands,in.whitePoint,in.mvScaleX,in.mvScaleY,
            in.codecColor,in.codecAlpha,2,in.workProxy,in.modelB,in.modelC};
        const auto r=c.record(customIn);
        expect(r.state==CoreState::Recorded && r.passes==2 && p.recordedClamps==1,"custom pass2 chain");
    }
    for(uint32_t count:{1u,2u}) {
        // Given contiguous ready histories at native extent.
        Probe p;auto c=core(p);
        // When one job is recorded.
        const auto r=c.record(fixture({count,2}));
        // Then only the immutable base and clamped answers feed alternating A/B outputs.
        const std::vector<int> order=count==1?std::vector<int>{1,20,5}:std::vector<int>{1,20,30,21,5};
        const std::vector<uint32_t> indices=count==1?std::vector<uint32_t>{0}:std::vector<uint32_t>{0,1};
        expect(r.state==CoreState::Recorded && r.evaluated && r.composed && r.passes==count &&
            r.passError==0 && r.failedPass==0 && r.error==0xff+count && p.calls==order && p.indices==indices &&
            p.recordedClamps==count-1 && p.source==&handles[1] && p.answer==&handles[count%2?3:6],"pass chain");
    }
}
void failures() {
    for(uint32_t pass:{1u,2u}) {
        // Given an evaluator that fails at this one-based history.
        Probe p;p.failEval=pass;auto c=core(p);
        // When a three-pass job reaches that history.
        const auto r=c.record(fixture());
        // Then no answer is resolved and completed histories are reported precisely.
        expect(r.state==CoreState::EvaluationError && r.error==0xbad && r.failedPass==pass &&
            r.passes==pass-1 && r.evaluated==(pass>1) && !r.composed &&
            p.calls.back()==19+int(pass) && p.source==nullptr,"evaluation error resolved or wrong index");
    }
    for(uint32_t pass:{1u,2u}) {
        // Given a history that is not ready despite the caller's ready count.
        Probe p;p.notReady=pass;auto c=core(p);
        // When the job reaches it.
        const auto r=c.record(fixture());
        // Then no resolve is attempted.
        expect(r.state==CoreState::NotReady && r.passes==pass-1 && r.failedPass==pass &&
            r.error==0xabc && !r.composed && p.source==nullptr,"not-ready resolved");
    }
    for(uint32_t slot:{1u}) {
        // Given a codec refusing an inter-pass clamp before any commands.
        Probe p;p.refuseClamp=slot;auto c=core(p);
        // When the preceding answer completed.
        const auto r=c.record(fixture());
        // Then its untouched A/B answer is composed exactly once; the next history is marked skipped.
        expect(r.state==CoreState::Recorded && r.passes==slot && r.failedPass==slot+1 &&
            r.passError==core_clamp_refused && r.composed && p.recordedClamps==slot-1 &&
            p.answer==&handles[slot%2?3:6] && p.calls.back()==5,"clamp refusal lost intact answer");
    }
    {
        // Given all evaluations succeed but the final codec refuses.
        Probe p;p.resolve=false;auto c=core(p);
        // When one three-pass job is recorded.
        const auto r=c.record(fixture());
        // Then completed evaluations do not imply composition or publication.
        expect(r.state==CoreState::ResolveError && r.passes==2 && r.evaluated && !r.composed &&
            r.error==0 && r.passError==0 && p.calls.back()==5,"resolve refusal publication");
    }
}
void scaled() {
    for(uint32_t scale:{50u,55u,75u,100u}) {
        // Given a valid working-size A/B/C set.
        Probe p;auto c=core(p);
        // When three passes run.
        const auto r=c.record(fixture({2,2,scale}));
        // Then scale reduction is recorded once, before any evaluation; resolve uses the same base.
        const std::vector<int> expected=scale<100?std::vector<int>{1,4,20,30,21,5}:
            std::vector<int>{1,20,30,21,5};
        expect(r.state==CoreState::Recorded && p.calls==expected && p.source==&handles[scale<100?5:1],
            "scaled chain order/source");
    }
    {
        // Given a refused downsample.
        Probe p;p.downsample=false;auto c=core(p);
        // When a reduced job is recorded.
        const auto r=c.record(fixture({2,2,50}));
        // Then no model or resolve sees an uninitialized work proxy.
        expect(r.state==CoreState::EncodeError && r.passes==0 && p.calls==std::vector<int>{1,4},"downsample refusal");
    }
}
void boundaries() {
    for(Spec s:std::vector<Spec>{{0,2},{3,2},{2,1},{1,0},{1,3},{1,1,49},{1,1,101},
        {2,2,50,Fault::WorkMissing},{2,2,50,Fault::WorkExtent},{2,2,50,Fault::ModelExtent},
        {2,2,100,Fault::BExtent},{2,2,100,Fault::CMissing},{2,2,100,Fault::BCAlias},{2,2,50,Fault::WorkAlias}}) {
        // Given an invalid count, active texture, extent or alias.
        Probe p;auto c=core(p);
        // When admission is attempted.
        const auto r=c.record(fixture(s));
        // Then rejection precedes every codec/evaluator call.
        expect(r.state==CoreState::InvalidInput && r.passes==0 && p.calls.empty(),"invalid input recorded commands");
    }
    {
        // Given unused optional handles are invalid for native one-pass.
        const auto in=fixture({1,1});
        const CoreInput minimal{in.job,in.epochs,in.resources,in.history,in.commands,in.whitePoint,in.mvScaleX,in.mvScaleY,
            in.codecColor,in.codecAlpha};
        Probe p;auto c=core(p);
        // When the legacy aggregate is recorded.
        const auto r=c.record(minimal);
        // Then it needs no optional allocation.
        expect(r.state==CoreState::Recorded && r.passes==1 && p.calls==std::vector<int>{1,20,5},"optional handles required");
    }
}
}
int main() try {
    chains();failures();scaled();boundaries();
    std::puts("PASS core multipass: 3 chains, 3 evaluation errors, 3 readiness errors, 2 safe clamp refusals, final resolve refusal, 4 scales, downsample refusal, 14 admission refusals, legacy optional handles");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
