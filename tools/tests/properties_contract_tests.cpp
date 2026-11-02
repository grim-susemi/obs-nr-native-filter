// SPDX-License-Identifier: GPL-3.0-only
#include "nr_properties.h"
#include "nr_core.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace nr::properties;
void expect(bool value,const char* reason) { if(!value)throw std::runtime_error(reason); }
unsigned inspections=0;
nr::runtime::Inspection count_inspection(const nr::runtime::RuntimeIdentity& identity)
{
    ++inspections;return nr::runtime::inspect(identity);
}
struct CpuProcessor final:nr::pipeline::Processor {
    nr::pipeline::Processed process(const nr::pipeline::Work& w,std::stop_token)override {return {w.job,false,{}};}
};
void decoded_toggle_lifecycle(const std::shared_ptr<const nr::Settings>& initial)
{
    nr::pipeline::FramePipeline pipeline({91,1,1,1},std::make_shared<CpuProcessor>());
    nr::lifecycle::Observer observer(pipeline);
    const nr::FrameDescriptor descriptor{{16,16},{0,0,16,16},nr::PixelFormat::Rgba8,
        nr::ColorSpace::Rec709Srgb,nr::AlphaMode::Premultiplied,80};
    const auto observation=[&](uint64_t tick,const nr::Settings& settings) {
        return nr::lifecycle::Observation{tick,11,22,nr::SourceValidity::Valid,descriptor,settings};
    };
    const auto sample=[&](const nr::lifecycle::Observation& o) {
        const auto e=pipeline.epochs();
        return nr::IngressSample{{91,e.contentEpoch,o.canonicalTick},o.canonicalTick,o.canonicalTick-1,
            11,22,e.resourceGeneration,o.validity,descriptor,o.settings};
    };
    expect(!observer.prepare(observation(10,*initial)).placeholder,"initial lifecycle admitted");
    auto* controlsData=obs_data_create();defaults(controlsData);obs_data_set_bool(controlsData,"nr_enabled",false);
    auto disabled=updated(initial,decode(controlsData,default_runtime,initial->runtimePath));
    const auto off=observation(11,*disabled);
    const auto cutoff=observer.prepare(off);
    expect(cutoff.placeholder && cutoff.placeholder->kind==nr::OutputKind::CurrentSource &&
        !cutoff.placeholder->frame && !cutoff.placeholder->job,"disabled discards old keys");
    const auto offPlan=pipeline.begin(sample(off),12);
    expect(!offPlan.admission && pipeline.finish(nullptr),"disabled no submission/capture");
    const auto counters=pipeline.counters();
    expect(counters.captures==0 && counters.dispatches==0,"disabled counters zero");
    obs_data_set_bool(controlsData,"nr_enabled",true);
    auto enabled=updated(disabled,decode(controlsData,default_runtime,initial->runtimePath));
    obs_data_release(controlsData);
    const auto on=observation(12,*enabled);
    const auto restarted=observer.prepare(on);
    expect(!restarted.placeholder && restarted.resetTemporal,"reenable resets histories");
    const auto input=sample(on);
    const auto plan=pipeline.begin(input,13);
    expect(plan.selection.kind==nr::OutputKind::PrimingRaw,"reenable primes no stale output");
    expect(pipeline.finish(std::make_shared<const nr::pipeline::RawFrame>(nr::pipeline::RawFrame{input,
        std::make_shared<const unsigned>(7)})),"reenable raw latch");
    const auto cleanup=observation(13,*disabled);
    (void)observer.prepare(cleanup);(void)pipeline.begin(sample(cleanup),14);
    expect(pipeline.finish(nullptr),"release raw before close");
    const auto exit=pipeline.worker_exit();pipeline.request_stop();
    expect(exit.wait_for(std::chrono::seconds(5))==std::future_status::ready && pipeline.try_close(),"CPU lifecycle closes");
}
int main()
{
    try {
        auto* data=obs_data_create();
        const std::u8string utf8=u8"C:/owned/\uD14C\uC2A4\uD2B8/runtime.dll";
        expect(utf8_path(std::string(reinterpret_cast<const char*>(utf8.data()),utf8.size())).generic_u8string()==utf8,"UTF8 path roundtrip");
        auto* unicodeData=obs_data_create();defaults(unicodeData);
        const std::string unicodePath(reinterpret_cast<const char*>(utf8.data()),utf8.size());
        obs_data_set_string(unicodeData,"nr_runtime_path",unicodePath.c_str());
        obs_data_set_bool(unicodeData,"nr_enabled",false);obs_data_set_double(unicodeData,"nr_strength",.25);
        (void)admit(unicodeData,count_inspection);
        auto* serialized=obs_data_create_from_json(obs_data_get_json(unicodeData));
        expect(serialized && std::string(obs_data_get_string(serialized,"nr_runtime_path"))==unicodePath &&
            !obs_data_get_bool(serialized,"nr_enabled") && obs_data_get_double(serialized,"nr_strength")==.25,
            "CPU obs_data JSON preserves original UTF8/user values; not OBS source restart proof");
        obs_data_release(serialized);obs_data_release(unicodeData);
        defaults(data);
        auto* props=controls({"priming","awaitingSample"});
        expect(obs_property_get_type(obs_properties_get(props,"nr_enabled"))==OBS_PROPERTY_BOOL,"boolean property");
        expect(obs_property_get_type(obs_properties_get(props,"nr_runtime_path"))==OBS_PROPERTY_PATH &&
            obs_property_path_type(obs_properties_get(props,"nr_runtime_path"))==OBS_PATH_FILE,"file property");
        auto* slider=obs_properties_get(props,"nr_strength");
        expect(obs_property_get_type(slider)==OBS_PROPERTY_FLOAT && obs_property_float_type(slider)==OBS_NUMBER_SLIDER &&
            obs_property_float_min(slider)==0 && obs_property_float_max(slider)==1 && obs_property_float_step(slider)==.01,"slider bounds");
        for(const auto* key:{"nr_status","nr_reason"})expect(!obs_property_enabled(obs_properties_get(props,key)) &&
            obs_property_text_type(obs_properties_get(props,key))==OBS_TEXT_INFO,"read-only status");
        obs_properties_destroy(props);
        const auto first=admit(data,count_inspection);
        expect(first.enabled && first.strength.value==1 && first.runtimePath==default_runtime,"defaults");
        expect(effective_enabled(true,true) && !effective_enabled(true,false) &&
               !effective_enabled(false,true) && !effective_enabled(false,false),"enable truth table");
        auto a=updated({},first);
        decoded_toggle_lifecycle(a);
        expect(a->revision.value==1 && a->tuning==nr::core_default_tuning(),"initial revision/tuning");
        expect(updated(a,first)==a,"unchanged settings identity");
        auto custom=std::make_shared<const nr::Settings>(nr::Settings{{8},true,a->runtimePath,{.4f},{2,3,.2f,.3f,.4f,-1,false}});
        const auto preserved=updated(custom,{false,a->runtimePath,{.6f},{}});
        expect(preserved->tuning==custom->tuning && preserved->revision.value==9,"owned tuning preserved");
        obs_data_set_bool(data,"nr_enabled",false);
        obs_data_set_double(data,"nr_strength",0);
        obs_data_set_string(data,"foreign_owner","keep");
        const auto changed=decode(data,default_runtime,a->runtimePath);
        auto b=updated(a,changed);
        expect(!b->enabled && b->strength.value==0 && b->revision.value==2 && b->tuning==a->tuning,"relevant change once");
        expect(updated(b,changed)==b && std::string(obs_data_get_string(data,"foreign_owner"))=="keep","unrelated preserved");
        defaults(data);
        expect(!obs_data_get_bool(data,"nr_enabled") && obs_data_get_double(data,"nr_strength")==0,"defaults preserve overrides");
        const auto beforeUpdates=inspections;
        for(double v:{0.,1.}) { obs_data_set_double(data,"nr_strength",v);expect(decode(data,default_runtime,a->runtimePath).reason.empty(),"endpoint admitted"); }
        for(double v:{-.001,1.001,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            obs_data_set_double(data,"nr_strength",v);expect(decode(data,default_runtime,a->runtimePath).reason=="invalidStrength","invalid strength refused");
        }
        expect(inspections==beforeUpdates,"deferred update decoder performs no admission inspection");
        obs_data_set_double(data,"nr_strength",1);
        obs_data_set_string(data,"nr_runtime_path","relative.dll");
        expect(admit(data,count_inspection).reason=="runtimePathNotAbsolute","relative refused");
        expect(decode(data,default_runtime,a->runtimePath).reason=="runtimeChangeRequiresRecreate","deferred different path refuses without admission");
        obs_data_set_string(data,"nr_runtime_path","C:/nr-properties-contract-missing.dll");
        expect(admit(data,count_inspection).reason=="runtimeMissing","missing refused");
        obs_data_set_string(data,"nr_runtime_path","C:/Windows/System32/kernel32.dll");
        expect(admit(data,count_inspection).reason=="hashMismatch","wrong runtime hash refused");
        const auto refused=nr::runtime::inspect({std::filesystem::canonical("C:/Windows/System32/kernel32.dll"),runtime_sha256});
        expect(!refused.accepted && !refused.loaded,"admission never loads DLL");
        obs_data_set_string(data,"nr_runtime_path",default_runtime);
        const auto valid=admit(data,count_inspection);
        expect(valid.reason.empty() && decode(data,default_runtime,valid.runtimePath).runtimePath==valid.runtimePath,"canonical runtime identity");
        expect(decode(data,"C:/immutable-other-runtime.dll","C:/immutable-other-runtime.dll").reason=="runtimeChangeRequiresRecreate","runtime replacement refused");
        expect(status(nr::Status::Applied,nr::Reason::None).state=="ready","applied status");
        expect(status(nr::Status::Priming,nr::Reason::None).state=="priming","priming status");
        expect(status(nr::Status::Disabled,nr::Reason::None).reason=="disabled","disabled status");
        expect(status(nr::Status::Bypassed,nr::Reason::InvalidSettings).reason=="invalidSettings","typed bypass reason");
        expect(status(nr::Status::Error,nr::Reason::EvaluationFailure).state=="error","error status");
        obs_data_release(data);
        std::puts("properties_contract: PASS actual libobs defaults properties enable revision strength static admission status preservation; no OBS startup/GPU/DLL load");return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
