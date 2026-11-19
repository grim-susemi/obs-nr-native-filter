// SPDX-License-Identifier: GPL-3.0-only
#include "obs_native_output.h"
#include <cstdio>
#include <stdexcept>
using namespace nr;
namespace {
unsigned checks=0;
void expect(bool ok,const char* why) {++checks;if(!ok)throw std::runtime_error(why);}
void protected_extent(Extent size,uint32_t scale) {
    // Given a real NativeOutput, valid immutable settings and no graphics device.
    model::Config config;config.runtime.canonicalPath="C:/qa/not-loaded-runtime.dll";
    obs_adapter::NativeOutput output(7,config,{},{},{});
    const Settings settings{{1},true,config.runtime.canonicalPath.generic_string(),{1},core_default_tuning(),2,scale};
    const auto unchanged=settings;
    const FrameDescriptor d{size,{0,0,size.width,size.height},PixelFormat::Rgba8,ColorSpace::Rec709Srgb,AlphaMode::Straight,80};
    const lifecycle::Observation input{10,11,12,SourceValidity::Valid,d,settings};
    // When the actual native preparation boundary observes a protected work size.
    const auto decision=output.prepare(input,nullptr);
    // Then it selects current raw without constructing a generation, preparing a model or dispatching a job.
    expect(decision.placeholder && decision.placeholder->kind==OutputKind::CurrentSource &&
        decision.placeholder->reason==Reason::WorkScaleTooSmall && decision.placeholder->status==Status::Bypassed,
        "protected work did not select original raw");
    expect(!output.context() && !output.transport() && !output.current_job() &&
        output.counters().dispatches==0 && output.counters().captures==0,"protected work prepared/submitted NR");
    expect(settings==unchanged && output.pass_status()==model::PassStatus{2,0,0,model::work_scale_refused},
        "guard changed saved settings or claimed 100-percent NR");
    const auto exited=output.worker_exit();output.request_stop();(void)output.try_close();
    expect(exited.valid() && exited.wait_for(std::chrono::seconds(5))==std::future_status::ready &&
        output.try_close(),"guarded owner did not close on exact worker exit");
}
}
int main() try {
    for(const auto& test:std::array<std::pair<Extent,uint32_t>,7>{{
            {{479,270},100},{{480,269},100},{{479,269},100},{{320,180},100},
            {{64,64},100},{{958,540},50},{{960,538},50}}})
        protected_extent(test.first,test.second);
    {
        // Given an old generation's coherent failure snapshot.
        const model::PassStatus old{2,1,2,0xbad00001u};
        // When the adapter binds it to the current requested generation.
        const auto same=obs_adapter::adapter_pass_status(old,2,true);
        const auto replaced=obs_adapter::adapter_pass_status(old,2,false);
        const auto changed=obs_adapter::adapter_pass_status(old,1,true);
        // Then it either preserves all four fields or publishes a fresh zero-count snapshot.
        expect(same==old && replaced==model::PassStatus{2,0,0,0} &&
            changed==model::PassStatus{1,0,0,0},"adapter mixed requested and retired generation status");
    }
    {
        // Given one guarded valid request followed by rejected settings.
        model::Config config;config.runtime.canonicalPath="C:/qa/not-loaded-runtime.dll";
        obs_adapter::NativeOutput output(8,config,{},{},{});
        const FrameDescriptor d{{479,270},{0,0,479,270},PixelFormat::Rgba8,ColorSpace::Rec709Srgb,AlphaMode::Straight,80};
        const Settings valid{{1},true,config.runtime.canonicalPath.generic_string(),{1},core_default_tuning(),2,100};
        (void)output.prepare({10,11,12,SourceValidity::Valid,d,valid},nullptr);
        const Settings invalid{{2},true,valid.runtimePath,{1},core_default_tuning(),3,100};
        // When the actual boundary rejects an out-of-range request.
        const auto refused=output.prepare({11,11,12,SourceValidity::Valid,d,invalid},nullptr);
        // Then an invalid count cannot corrupt the packed status or prepare NR.
        expect(refused.placeholder && refused.placeholder->reason==Reason::InvalidSettings &&
            output.pass_status()==model::PassStatus{2,0,0,0} && !output.context() && !output.current_job() &&
            invalid.passes==3,"rejected settings corrupted pass status or changed stored request");
        const auto exited=output.worker_exit();output.request_stop();(void)output.try_close();
        expect(exited.wait_for(std::chrono::seconds(5))==std::future_status::ready && output.try_close(),"status boundary cleanup");
    }
    // Given the exact conservative boundary, using only a fake preparation/submission counter.
    const Settings settings{{1},true,"fake",{1},core_default_tuning(),2,50};
    unsigned prepare=0,submit=0;
    // When the same production policy admits 480x270.
    if(model::work_admitted(work_extent({0,0,960,540},settings.workScale))){++prepare;++submit;}
    // Then fake admission is allowed, without a physical NGX or minimum-size claim.
    expect(prepare==1 && submit==1 && settings.passes==2 && settings.workScale==50,"fake policy boundary rejected");
    std::printf("PASS model_work_guard checks=%u protected_cases=7 prepare=0 submit=0 raw=true boundary480x270=fake-only vendor=false\n",checks);
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s checks=%u\n",e.what(),checks);return 1;}
