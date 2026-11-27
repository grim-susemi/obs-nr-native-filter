// SPDX-License-Identifier: GPL-3.0-only
#include "nr_properties.h"
#include "nr_core.h"
#include <obs-module.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>
using namespace nr::properties;
// The plugin resolves obs_module_text through OBS_MODULE_USE_DEFAULT_LOCALE, and libobs
// exports no such symbol; this executable mirrors that macro's miss path, returning the key.
const char* obs_module_text(const char* lookup) {return lookup;}
void expect(bool value,const char* reason) { if(!value)throw std::runtime_error(reason); }
#include "passes_properties_tests.inc"
#include "nr_hotfix_properties_tests.inc"
unsigned inspections=0;
nr::runtime::Inspection count_inspection(const nr::runtime::RuntimeIdentity& identity)
{
    ++inspections;return nr::runtime::inspect(identity);
}
nr::runtime::Inspection accept_path(const nr::runtime::RuntimeIdentity&)
{
    nr::runtime::Inspection result{};
    result.accepted=true;
    return result;
}
nr::runtime::Inspection accept_310_9(const nr::runtime::RuntimeIdentity&)
{
    ++inspections;
    nr::runtime::Inspection result{};
    result.reason="hashMismatch";
    result.sha256="cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed";
    return result;
}
nr::runtime::Inspection reject_modified(const nr::runtime::RuntimeIdentity&)
{
    ++inspections;
    nr::runtime::Inspection result{};
    result.reason="hashMismatch";
    result.sha256=std::string(64,'0');
    return result;
}
void dual_runtime_admission(const std::filesystem::path& fixture)
{
    auto* data=obs_data_create();defaults(data);
    obs_data_set_string(data,"nr_runtime_path",fixture.generic_string().c_str());
    const auto before=inspections;
    const auto modern=admit(data,accept_310_9);
    expect(modern.reason.empty() && modern.runtimeSha256=="cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed" &&
        modern.runtimeSupportsModelB,"310.9.16.0 digest accepted from the pinned table");
    expect(inspections==before+1,"310.9.16.0 inspected exactly once");
    const auto rejected=admit(data,reject_modified);
    expect(rejected.reason=="hashMismatch" && rejected.runtimeSha256.empty() && !rejected.runtimeSupportsModelB,
        "unnlisted/modified digest refused");
    expect(inspections==before+2,"modified digest inspected exactly once");
    std::puts("admission: 310.9.16.0 digest accepted, modified digest hashMismatch, one inspection each, zero load");
    obs_data_release(data);
}
struct LocalizedLabel {const char* text;const char* key;};
void localized_label_mapping()
{
    const Labels defaults;
    const auto localized=localized_labels();
    expect(localized.perfModel!=defaults.perfModel,"perf model label comes from obs_module_text, not the hardcoded default");
    expect(localized.hintPerfModel!=defaults.hintPerfModel,"perf model hint comes from obs_module_text");
    expect(localized.dedupReuse!=defaults.dedupReuse,"dedup reuse label comes from obs_module_text");
    expect(localized.hintDedupReuse!=defaults.hintDedupReuse,"dedup reuse hint comes from obs_module_text");
    expect(localized.runtimeInfo!=defaults.runtimeInfo,"runtime info label comes from obs_module_text");
    for(const LocalizedLabel& label:{LocalizedLabel{localized.perfModel,"NR.PerfModel"},
            LocalizedLabel{localized.hintPerfModel,"NR.Hint.PerfModel"},LocalizedLabel{localized.dedupReuse,"NR.DedupReuse"},
            LocalizedLabel{localized.hintDedupReuse,"NR.Hint.DedupReuse"},LocalizedLabel{localized.runtimeInfo,"NR.RuntimeInfo"}})
        expect(label.text && std::string(label.text)==label.key,"localized label resolves its exact locale key");
    expect(defaults.perfModelUnavailable && *defaults.perfModelUnavailable &&
        std::string(defaults.perfModelUnavailable)!=defaults.perfModel,"unavailable explanation has a distinct English fallback");
    std::puts("localized_labels: 5 NR labels resolve through obs_module_text (key echo stub), unavailable fallback present, not the hardcoded defaults");
}
std::filesystem::path locale_dir()
{
    std::error_code ec;
    auto dir=std::filesystem::current_path(ec);
    for(int depth=0;!ec && depth<6;++depth) {
        if(std::filesystem::exists(dir/"data"/"locale"/"en-US.ini",ec))return dir/"data"/"locale";
        if(!dir.has_parent_path()||dir.parent_path()==dir)break;
        dir=dir.parent_path();
    }
    return {};
}
void locale_keys()
{
    const auto dir=locale_dir();
    if(dir.empty()) {
        std::puts("SKIP locale keys: data/locale/en-US.ini not reachable from the test working directory");
        return;
    }
    const char* keys[]={"NR.PerfModel","NR.Hint.PerfModel","NR.PerfModel.Unavailable",
        "NR.DedupReuse","NR.Hint.DedupReuse","NR.RuntimeInfo"};
    for(const char* locale:{"en-US.ini","ko-KR.ini"}) {
        std::ifstream file(dir/locale);
        if(!file) {
            std::printf("SKIP locale keys: cannot read data/locale/%s\n",locale);
            return;
        }
        std::vector<std::string> lines;
        for(std::string line;std::getline(file,line);)lines.push_back(line);
        for(const char* key:keys) {
            const std::string prefix=std::string(key)+"=";
            bool present=false;
            for(const auto& line:lines) {
                if(line.rfind(prefix,0)!=0)continue;
                auto value=line.substr(prefix.size());
                while(!value.empty() && (value.back()=='\r' || value.back()==' '))value.pop_back();
                if(!value.empty() && value!="\"\"") {present=true;break;}
            }
            const std::string reason=std::string(key)+" missing or empty in data/locale/"+locale;
            expect(present,reason.c_str());
        }
    }
    std::puts("locale keys: 6/6 NR Model B/dedup/runtime keys present with values in en-US.ini and ko-KR.ini");
}
void model_b_controls(const std::shared_ptr<const nr::Settings>& initial)
{
    auto* data=obs_data_create();defaults(data);
    expect(!obs_data_get_bool(data,"nr_perf_model") && !obs_data_get_bool(data,"nr_dedup_reuse"),
        "performance model and dedup reuse default off");
    obs_data_set_bool(data,"nr_perf_model",true);obs_data_set_bool(data,"nr_dedup_reuse",true);
    const auto decoded=decode(data,default_runtime,initial->runtimePath);
    expect(decoded.reason.empty() && decoded.perfModel && decoded.dedupReuse,"performance model decode");
    const auto next=updated(initial,decoded);
    expect(next->perfModel && next->dedupReuse && next->revision.value==initial->revision.value+1,
        "performance model revision once");
    expect(updated(next,decoded)==next,"performance model identity");
    auto* props=controls({"priming","awaitingSample"});
    for(const auto* key:{"nr_perf_model","nr_dedup_reuse"})
        expect(obs_properties_get(props,key) && obs_property_get_type(obs_properties_get(props,key))==OBS_PROPERTY_BOOL,key);
    auto* info=obs_properties_get(props,"nr_runtime_info");
    expect(info && obs_property_get_type(info)==OBS_PROPERTY_TEXT && !obs_property_enabled(info),"read-only runtime info");
    auto* row=obs_properties_get(props,"nr_work_scale");
    expect(obs_property_next(&row) && std::string(obs_property_name(row))=="nr_perf_model" &&
        obs_property_next(&row) && std::string(obs_property_name(row))=="nr_dedup_reuse" &&
        obs_property_next(&row) && std::string(obs_property_name(row))=="nr_runtime_info","performance control order");
    expect(!obs_properties_get(props,"nr_perf_model_unavailable"),"available Model B hides the unavailable row");
    obs_properties_destroy(props);
    Labels unavailable;unavailable.perfModelAvailable=false;unavailable.perfModelUnavailable="U-unavailable";
    auto* gated=controls({},unavailable);
    auto* gatedPerf=obs_properties_get(gated,"nr_perf_model");
    expect(gatedPerf && !obs_property_enabled(gatedPerf),"310.8 Model B control disabled");
    expect(std::string(obs_property_long_description(gatedPerf))=="U-unavailable","disabled Model B tooltip explains why");
    auto* note=obs_properties_get(gated,"nr_perf_model_unavailable");
    expect(note && obs_property_get_type(note)==OBS_PROPERTY_TEXT && !obs_property_enabled(note) &&
        std::string(obs_property_description(note))=="U-unavailable","Model B unavailable row exact text and disabled state");
    auto* afterRuntime=obs_properties_get(gated,"nr_runtime_info");
    expect(obs_property_next(&afterRuntime) && std::string(obs_property_name(afterRuntime))=="nr_perf_model_unavailable",
        "unavailable row follows the runtime info row");
    std::puts("model_b_controls: 310.8 Model B checkbox disabled, tooltip 'U-unavailable', nr_perf_model_unavailable TEXT row disabled with 'U-unavailable' after nr_runtime_info, available Model B hides the row");
    obs_properties_destroy(gated);
    localized_label_mapping();
    locale_keys();
    obs_data_release(data);
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
void tuning_controls(const std::shared_ptr<const nr::Settings>& initial)
{
    auto* data=obs_data_create();defaults(data);
    expect(obs_data_get_int(data,"nr_preset")==0 && obs_data_get_int(data,"nr_style")==0 &&
        obs_data_get_double(data,"nr_intensity")==1 && obs_data_get_double(data,"nr_local_structure")==1 &&
        obs_data_get_double(data,"nr_local_tone")==0 && obs_data_get_double(data,"nr_skin_structure")==-1 &&
        obs_data_get_bool(data,"nr_auto_mask"),"tuning defaults match core defaults except OBS local tone 0 (#768)");
    const auto base=decode(data,default_runtime,initial->runtimePath);
    expect(base.reason.empty() && base.tuning==nr::ModelTuning{0,0,1,1,0,-1,true},"default tuning decoded");
    auto* props=controls({"priming","awaitingSample"});
    struct Slider {const char* key;double low,high,step;};
    for(const Slider& s:{Slider{"nr_intensity",0,2,.05},Slider{"nr_local_structure",0,2,.05},
            Slider{"nr_local_tone",0,2,.05},Slider{"nr_skin_structure",-1,2,.05}}) {
        auto* p=obs_properties_get(props,s.key);
        expect(p && obs_property_get_type(p)==OBS_PROPERTY_FLOAT && obs_property_float_type(p)==OBS_NUMBER_SLIDER &&
            obs_property_float_min(p)==s.low && obs_property_float_max(p)==s.high && obs_property_float_step(p)==s.step,s.key);
    }
    auto* preset=obs_properties_get(props,"nr_preset");
    expect(preset && obs_property_get_type(preset)==OBS_PROPERTY_INT && obs_property_int_type(preset)==OBS_NUMBER_SLIDER &&
        obs_property_int_min(preset)==0 && obs_property_int_max(preset)==3 && obs_property_int_step(preset)==1,"preset slider bounds");
    auto* style=obs_properties_get(props,"nr_style");
    expect(style && obs_property_get_type(style)==OBS_PROPERTY_LIST && obs_property_list_format(style)==OBS_COMBO_FORMAT_INT &&
        obs_property_list_item_count(style)==3 && obs_property_list_item_int(style,0)==0 &&
        obs_property_list_item_int(style,1)==1 && obs_property_list_item_int(style,2)==2,"style list");
    expect(obs_property_get_type(obs_properties_get(props,"nr_auto_mask"))==OBS_PROPERTY_BOOL,"auto mask checkbox");
    obs_properties_destroy(props);
    obs_data_set_int(data,"nr_preset",3);obs_data_set_int(data,"nr_style",2);
    obs_data_set_double(data,"nr_intensity",1.5);obs_data_set_double(data,"nr_local_structure",.5);
    obs_data_set_double(data,"nr_local_tone",2);obs_data_set_double(data,"nr_skin_structure",0);
    obs_data_set_bool(data,"nr_auto_mask",false);
    const auto edited=decode(data,default_runtime,initial->runtimePath);
    expect(edited.reason.empty() && edited.tuning==nr::ModelTuning{3,2,1.5f,.5f,2.f,0.f,false},"edited tuning decoded");
    const auto next=updated(initial,edited);
    expect(next->revision.value==initial->revision.value+1 && next->tuning==edited.tuning &&
        next->enabled==initial->enabled && next->runtimePath==initial->runtimePath &&
        next->strength==initial->strength,"tuning change advances revision once");
    expect(updated(next,edited)==next,"unchanged tuning identity");
    struct Edge {const char* key;double value;bool valid;};
    for(const Edge& e:{Edge{"nr_intensity",0,true},Edge{"nr_intensity",2,true},Edge{"nr_skin_structure",-1,true},
            Edge{"nr_skin_structure",2,true},Edge{"nr_intensity",-.01,false},Edge{"nr_intensity",2.01,false},
            Edge{"nr_local_structure",2.01,false},Edge{"nr_local_tone",-.01,false},Edge{"nr_skin_structure",-1.01,false},
            Edge{"nr_skin_structure",2.01,false},Edge{"nr_intensity",std::numeric_limits<double>::quiet_NaN(),false},
            Edge{"nr_local_tone",std::numeric_limits<double>::infinity(),false}}) {
        auto* probe=obs_data_create();defaults(probe);obs_data_set_double(probe,e.key,e.value);
        const auto reason=decode(probe,default_runtime,initial->runtimePath).reason;
        expect(e.valid?reason.empty():reason=="invalidTuning",e.key);
        obs_data_release(probe);
    }
    struct IntEdge {const char* key;long long value;bool valid;};
    for(const IntEdge& e:{IntEdge{"nr_preset",0,true},IntEdge{"nr_preset",3,true},IntEdge{"nr_style",0,true},
            IntEdge{"nr_style",2,true},IntEdge{"nr_preset",-1,false},IntEdge{"nr_preset",4,false},
            IntEdge{"nr_style",-1,false},IntEdge{"nr_style",3,false}}) {
        auto* probe=obs_data_create();defaults(probe);obs_data_set_int(probe,e.key,e.value);
        const auto reason=decode(probe,default_runtime,initial->runtimePath).reason;
        expect(e.valid?reason.empty():reason=="invalidTuning",e.key);
        obs_data_release(probe);
    }
    obs_data_release(data);
}
void multipass_controls(const std::shared_ptr<const nr::Settings>& initial)
{
    // Given: legacy OBS data without either new key, even without registered defaults.
    auto* data=obs_data_create();defaults(data);
    expect(obs_data_get_int(data,"nr_passes")==1 && obs_data_get_int(data,"nr_work_scale")==100,"multipass defaults");
    obs_data_unset_default_value(data,"nr_passes");obs_data_unset_default_value(data,"nr_work_scale");
    // When: decoding the legacy data.
    const auto legacy=decode(data,default_runtime,initial->runtimePath);
    // Then: old settings retain the default path.
    expect(legacy.reason.empty() && legacy.passes==1 && legacy.workScale==100,"legacy multipass defaults");
    for(const auto& values:std::array<std::array<uint32_t,2>,3>{{{2,100},{1,75},{2,50}}}) {
        // Given: independently changed pass/scale values, including simultaneous changes.
        obs_data_set_int(data,"nr_passes",values[0]);obs_data_set_int(data,"nr_work_scale",values[1]);
        const auto decoded=decode(data,default_runtime,initial->runtimePath);
        // When: updating the immutable settings.
        const auto next=updated(initial,decoded);
        // Then: values survive, with exactly one revision and stable identity on repeated updates.
        expect(decoded.reason.empty() && next->passes==values[0] && next->workScale==values[1] &&
            next->revision.value==initial->revision.value+1 && updated(next,decoded)==next,"multipass update revision once");
    }
    struct Edge {const char* key;long long value;const char* reason;};
    for(const auto& edge:{Edge{"nr_passes",0,"invalidPasses"},Edge{"nr_passes",3,"invalidPasses"},
            Edge{"nr_work_scale",49,"invalidWorkScale"},Edge{"nr_work_scale",101,"invalidWorkScale"}}) {
        // Given: one out-of-range value in actual obs_data.
        auto* invalid=obs_data_create();defaults(invalid);obs_data_set_int(invalid,edge.key,edge.value);
        // When: decoding it.
        const auto refused=decode(invalid,default_runtime,initial->runtimePath);
        // Then: the reason is specific and safe defaults remain, without rewriting user data.
        expect(refused.reason==edge.reason && refused.passes==1 && refused.workScale==100 &&
            obs_data_get_int(invalid,edge.key)==edge.value,"invalid multipass rejected with defaults");
        if(edge.value==3)std::puts("invalid obs_data nr_passes=3: reason=invalidPasses decoded=1/100 user_value=3");
        obs_data_release(invalid);
    }
    auto* props=controls({"priming","awaitingSample"});
    struct Slider {const char* key;int low,high,step;};
    for(const auto& s:{Slider{"nr_passes",1,2,1},Slider{"nr_work_scale",50,100,5}}) {
        auto* p=obs_properties_get(props,s.key);
        expect(p && obs_property_get_type(p)==OBS_PROPERTY_INT && obs_property_int_type(p)==OBS_NUMBER_SLIDER &&
            obs_property_int_min(p)==s.low && obs_property_int_max(p)==s.high && obs_property_int_step(p)==s.step,s.key);
    }
    expect(std::string(obs_property_int_suffix(obs_properties_get(props,"nr_work_scale")))=="%","work scale suffix");
    auto* next=obs_properties_get(props,"nr_strength");
    expect(obs_property_next(&next) && std::string(obs_property_name(next))=="nr_passes" &&
        obs_property_next(&next) && std::string(obs_property_name(next))=="nr_work_scale","multipass control order");
    {
        auto* p2data=obs_data_create();defaults(p2data);
        obs_data_set_int(p2data,"nr_passes",2);
        obs_data_set_int(p2data,"nr_style",0);
        obs_data_set_bool(p2data,"nr_pass2_custom",true);
        obs_data_set_int(p2data,"nr_pass2_style",2);
        obs_data_set_double(p2data,"nr_pass2_intensity",1.5);
        obs_data_set_double(p2data,"nr_pass2_local_tone",0.25);
        const auto decoded2=decode(p2data,default_runtime,initial->runtimePath);
        expect(decoded2.reason.empty() && decoded2.pass2Tuning.has_value() &&
            decoded2.tuning.style==0 && decoded2.pass2Tuning->style==2 &&
            decoded2.pass2Tuning->intensity==1.5f && decoded2.pass2Tuning->localTone==0.25f,
            "custom pass2 decode");
        const auto next2=updated(initial,decoded2);
        expect(next2->pass2Tuning==decoded2.pass2Tuning && updated(next2,decoded2)==next2,"pass2 revision stability");
        obs_data_set_int(p2data,"nr_pass2_style",9);
        expect(decode(p2data,default_runtime,initial->runtimePath).reason=="invalidTuning","invalid pass2 style rejected");
        obs_data_release(p2data);
    }
    {
        auto* sdata=obs_data_create();defaults(sdata);
        const auto def=decode(sdata,default_runtime,initial->runtimePath);
        expect(def.reason.empty() && def.stabilize.temporalStrength==0.45f &&
            def.stabilize.holdStrength==0.45f && def.stabilize.holdTolerance==0.02f,
            "anti-flicker defaults");
        obs_data_set_bool(sdata,"nr_hold_enabled",false);
        obs_data_set_double(sdata,"nr_temporal_stability",0.0);
        const auto off=decode(sdata,default_runtime,initial->runtimePath);
        expect(off.reason.empty() && !off.stabilize.active(),"anti-flicker toggle off");
        obs_data_set_double(sdata,"nr_temporal_stability",1.5);
        expect(decode(sdata,default_runtime,initial->runtimePath).reason=="invalidTuning","invalid temporal rejected");
        obs_data_release(sdata);
        expect(obs_properties_get(props,"nr_temporal_stability") && obs_properties_get(props,"nr_hold_enabled") &&
            obs_properties_get(props,"nr_hold_strength") && obs_properties_get(props,"nr_hold_tolerance") &&
            obs_properties_get(props,"nr_quick_preset") && obs_properties_get(props,"nr_fps_overlay") &&
            obs_properties_get(props,"nr_grp_basic") && obs_properties_get(props,"nr_grp_stabilize"),
            "anti-flicker and UX/UI properties exposed");
    }
    obs_properties_destroy(props);obs_data_release(data);
}
void deadline_controls()
{
    struct Case {uint32_t total,late;std::optional<unsigned> percent;};
    for(const auto& test:{Case{300,0,{}},Case{300,27,{}},Case{300,29,{}},Case{0,0,{}},
            Case{300,30,10},Case{300,300,100},Case{300,600,100},Case{0,UINT32_MAX,{}},
            Case{UINT32_MAX,UINT32_MAX,100},Case{UINT32_MAX,UINT32_MAX/10,{}},Case{6,1,17}}) {
        // Given: an independently specified count boundary. When: deriving its warning.
        const auto warning=deadline_warning(test.total,test.late);
        // Then: threshold precedes rounded display and widened arithmetic saturates.
        expect(warning==test.percent,"deadline threshold rounding and saturation");
        Labels labels;if(warning)labels.deadlineText="warning";
        auto* props=controls({"ready","none"},labels);
        auto* row=obs_properties_get(props,"nr_deadline");
        expect((row!=nullptr)==warning.has_value(),"deadline row iff warning");
        if(row)expect(obs_property_text_type(row)==OBS_TEXT_INFO && !obs_property_enabled(row),"deadline row read-only");
        for(const auto* key:{"nr_status","nr_reason"})
            expect(obs_properties_get(props,key) && !obs_property_enabled(obs_properties_get(props,key)),"existing status rows retained");
        obs_properties_destroy(props);
    }
}
void deadline_windows()
{
    // Given: independent graphics-owned instances and synthetic monotonic video clocks.
    DeadlineWindow a,b;
    const StatusSnapshot ready{"ready","none"},priming{"priming","awaitingSample"};
    const std::array<StatusSnapshot,8> frames{{priming,{"bypassed","awaitingSettingsObservation"},ready,
        {"error","deadlineMiss"},{"bypassed","slotsFull"},{"bypassed","deadlineMiss"},
        {"priming","deadlineMiss"},{"ready","deadlineMiss"}}};
    for(size_t i=0;i<frames.size();++i) {
        expect(!a.observe(i+1,frames[i]) && !b.observe(i+1,priming),"deadline does not publish early");
    }
    expect(!a.observe(5000000000ull,ready),"deadline waits for full five seconds");
    // When: the exact five-second boundary closes each independent window.
    const auto first=a.observe(5000000001ull,ready),other=b.observe(5000000001ull,priming);
    // Then: only eligible ticks enter the denominator; only the two exact late keys enter late.
    expect(first && first->packed==((uint64_t{7}<<32)|2) && first->warning==29u && first->changed,
        "deadline exclusions exact late keys packed pair and on transition");
    expect(other && other->packed==0 && !other->warning && !other->changed,"priming-only instance remains quiet");
    expect(!a.observe(5000000002ull,ready),"next deadline window begins empty");
    const auto changedPercent=a.observe(10000000001ull,{"bypassed","deadlineMiss"});
    expect(changedPercent && changedPercent->packed==((uint64_t{2}<<32)|1) &&
        changedPercent->warning==50u && !changedPercent->changed,"percent update does not toggle warning");
    expect(!a.observe(10000000002ull,ready),"recovery window remains incomplete");
    const auto off=a.observe(15000000001ull,ready);
    expect(off && off->packed==(uint64_t{2}<<32) && !off->warning && off->changed,"warning off transition");
    const auto quiet=a.observe(20000000001ull,ready);
    expect(quiet && !quiet->warning && !quiet->changed,"warning off does not repeat");
    const auto again=a.observe(25000000001ull,{"bypassed","slotsFull"});
    expect(again && again->warning==100u && again->changed,"warning can turn on again");
    DeadlineWindow waiting;
    const StatusSnapshot awaiting{"bypassed","awaitingSettingsObservation"};
    expect(!waiting.observe(1,awaiting),"settings window starts without warning");
    const auto settingsOnly=waiting.observe(5000000001ull,awaiting);
    expect(settingsOnly && settingsOnly->packed==0 && !settingsOnly->warning,"settings-only window has no denominator");
}
int main(int argc,char** argv)
{
    try {
        if(argc!=1 && !(argc==2 && std::string_view(argv[1])=="--deadline-only"))
            throw std::runtime_error("unknown properties contract invocation");
        deadline_controls();deadline_windows();
        if(argc==2) {std::puts("deadline_contract: PASS thresholds actual OBS rows synthetic clock exclusions instances on-off transitions");return 0;}
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
        expect(!obs_properties_get(props,"nr_runtime_path"),"saved runtime path is not exposed as an editable property");
        auto* slider=obs_properties_get(props,"nr_strength");
        expect(obs_property_get_type(slider)==OBS_PROPERTY_FLOAT && obs_property_float_type(slider)==OBS_NUMBER_SLIDER &&
            obs_property_float_min(slider)==0 && obs_property_float_max(slider)==1 && obs_property_float_step(slider)==.01,"slider bounds");
        for(const auto* key:{"nr_status","nr_reason"})expect(!obs_property_enabled(obs_properties_get(props,key)) &&
            obs_property_text_type(obs_properties_get(props,key))==OBS_TEXT_INFO,"read-only status");
        {
            Labels shown;shown.statusText="S-localized";shown.reasonText="R-localized";
            auto* shownProps=controls({"bypassed","deadlineMiss"},shown);
            expect(std::string(obs_property_description(obs_properties_get(shownProps,"nr_status")))=="S-localized" &&
                   std::string(obs_property_description(obs_properties_get(shownProps,"nr_reason")))=="R-localized","status rows show localized text");
            obs_properties_destroy(shownProps);
            auto* rawProps=controls({"bypassed","deadlineMiss"});
            expect(std::string(obs_property_description(obs_properties_get(rawProps,"nr_status")))=="bypassed" &&
                   std::string(obs_property_description(obs_properties_get(rawProps,"nr_reason")))=="deadlineMiss","status rows fall back to raw codes");
            obs_properties_destroy(rawProps);
        }
        obs_properties_destroy(props);
        const auto installed=std::filesystem::canonical(argv[0]);
        dual_runtime_admission(installed);
        const auto first=admit(data,accept_path,installed);
        expect(first.enabled && first.strength.value==0.85f && first.reason.empty() &&
            first.runtimeSha256==runtime_sha256 && !first.runtimeSupportsModelB &&
            std::filesystem::equivalent(utf8_path(first.runtimePath),installed),"defaults resolve the installed runtime");
        expect(effective_enabled(true,true) && !effective_enabled(true,false) &&
               !effective_enabled(false,true) && !effective_enabled(false,false),"enable truth table");
        auto a=updated({},first);
        decoded_toggle_lifecycle(a);
        expect(a->revision.value==1 && a->tuning==nr::ModelTuning{0,0,1,1,0,-1,true},"initial revision/tuning (OBS local tone 0, #768)");
        expect(updated(a,first)==a,"unchanged settings identity");
        auto custom=std::make_shared<const nr::Settings>(nr::Settings{{8},true,a->runtimePath,{.4f},{2,3,.2f,.3f,.4f,-1,false}});
        const nr::ModelTuning chosen{1,2,.5f,1.5f,.25f,.75f,false};
        const auto followed=updated(custom,{false,a->runtimePath,{.6f},{},chosen});
        expect(followed->tuning==chosen && followed->revision.value==9,"tuning follows decoded values");
        tuning_controls(a);
        multipass_controls(a);
        model_b_controls(a);
        nr_hotfix_properties(a->runtimePath);
        passes_status_controls();
    {
        auto* udata = obs_data_create(); defaults(udata);
        const auto clean=updated({},decode(udata,default_runtime,a->runtimePath));
        obs_data_set_int(udata, "nr_sr_mode", 2);
        obs_data_set_int(udata, "nr_sr_model", 2);
        obs_data_set_double(udata, "nr_cas_sharpness", 1.0);
        obs_data_set_string(udata, "nr_sr_runtime_path", "C:/legacy/nvngx_dlss.dll");
        const auto legacy=decode(udata,default_runtime,a->runtimePath);
        expect(legacy.reason.empty() && updated(clean,legacy)==clean,
            "old 4K/CAS scene keys do not change the Native NR configuration");
        obs_data_set_int(udata, "nr_sr_mode", 99);
        const auto invalidLegacy=decode(udata,default_runtime,a->runtimePath);
        expect(invalidLegacy.reason.empty() && updated(clean,invalidLegacy)==clean,
            "invalid obsolete SR mode does not disable Native NR");
        auto* ui=controls({"ready","none"});
        for(const auto* key:{"nr_grp_upscale","nr_sr_mode","nr_sr_model","nr_cas_sharpness","nr_sr_runtime_path"})
            expect(!obs_properties_get(ui,key),"legacy upscale control is absent from Native NR");
        obs_properties_destroy(ui);
        obs_data_release(udata);
    }
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
        const auto wrongBefore=inspections;
        expect(admit(data,count_inspection).reason=="hashMismatch","wrong runtime hash refused");
        const auto refused=nr::runtime::inspect({std::filesystem::canonical("C:/Windows/System32/kernel32.dll"),runtime_sha256});
        expect(!refused.accepted && !refused.loaded,"admission never loads DLL");
        expect(inspections==wrongBefore+1,"wrong runtime inspected exactly once");
        std::puts("admission fixture: wrong/modified DLL -> hashMismatch, inspections=1, loaded=false");
        // This executable is a fixture for path selection only. The actual
        // proprietary admission and evaluation are checked in isolated OBS.
        const auto fixture=installed.generic_string();
        obs_data_set_string(data,"nr_runtime_path",default_runtime);
        const auto valid=admit(data,accept_path,installed);
        expect(valid.reason.empty() && !valid.runtimePath.empty() &&
            std::filesystem::equivalent(utf8_path(valid.runtimePath),installed) &&
            decode(data,default_runtime,valid.runtimePath).runtimePath==valid.runtimePath,"automatic installed runtime identity");
        obs_data_set_string(data,"nr_runtime_path","C:/nr-properties-contract-missing.dll");
        const auto stale=admit(data,accept_path,installed);
        expect(stale.reason.empty() && stale.runtimePath==valid.runtimePath,"missing old path recovers with installed runtime");
        obs_data_set_string(data,"nr_runtime_path",fixture.c_str());
        const auto manual=admit(data,accept_path,installed.parent_path()/L"unavailable.dll");
        expect(manual.reason.empty() && manual.runtimePath==valid.runtimePath,"valid existing path wins over automatic location");
        obs_data_set_string(data,"nr_runtime_path","C:/Windows/System32/kernel32.dll");
        expect(admit(data,count_inspection,installed).reason=="hashMismatch","wrong existing runtime never falls through");
        obs_data_set_string(data,"nr_runtime_path",default_runtime);
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
