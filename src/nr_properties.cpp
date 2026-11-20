// SPDX-License-Identifier: GPL-3.0-only
#include "nr_properties.h"
#include "nr_core.h"
namespace nr::properties {
std::optional<unsigned> deadline_warning(uint32_t total,uint32_t late)
{
    if(!total || uint64_t{late}*10<total)return std::nullopt;
    return static_cast<unsigned>(std::min(uint64_t{100},(uint64_t{late}*100+total/2)/total));
}
std::optional<DeadlineSnapshot> DeadlineWindow::observe(uint64_t now,const StatusSnapshot& snapshot)
{
    if(!start_)start_=now;
    if(snapshot.state!="priming" && snapshot.reason!="awaitingSettingsObservation") {
        ++total_;
        if(snapshot.state=="bypassed" && (snapshot.reason=="deadlineMiss" || snapshot.reason=="slotsFull"))++late_;
    }
    if(now-start_<5000000000ull)return std::nullopt;
    const auto warning=deadline_warning(total_,late_);
    const DeadlineSnapshot result{(uint64_t{total_}<<32)|late_,warning,warning.has_value()!=warning_};
    warning_=warning.has_value();start_=now;total_=late_=0;
    return result;
}
std::filesystem::path utf8_path(const std::string& value)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));
}
void defaults(obs_data_t* data)
{
    const auto tuning=core_default_tuning();
    obs_data_set_default_bool(data,"nr_enabled",true);
    obs_data_set_default_string(data,"nr_runtime_path",default_runtime);
    obs_data_set_default_double(data,"nr_strength",1);
    obs_data_set_default_int(data,"nr_passes",1);
    obs_data_set_default_int(data,"nr_work_scale",100);
    obs_data_set_default_int(data,"nr_preset",tuning.preset);
    obs_data_set_default_int(data,"nr_style",tuning.style);
    obs_data_set_default_double(data,"nr_intensity",tuning.intensity);
    obs_data_set_default_double(data,"nr_local_structure",tuning.localStructure);
    // Owner decision #768 A (2026-11-03): the OBS default keeps source brightness (local tone 0).
    // Vendor default 1 lifted shadows/green in copy-OBS ow30; core_default_tuning() stays the vendor value.
    obs_data_set_default_double(data,"nr_local_tone",0);
    obs_data_set_default_double(data,"nr_skin_structure",tuning.skinStructure);
    obs_data_set_default_bool(data,"nr_auto_mask",tuning.autoMask);
    obs_data_set_default_bool(data,"nr_pass2_custom",false);
    obs_data_set_default_int(data,"nr_pass2_preset",tuning.preset);
    obs_data_set_default_int(data,"nr_pass2_style",2); // Default 2nd pass suggestion: Cinematic (2) when custom enabled
    obs_data_set_default_double(data,"nr_pass2_intensity",tuning.intensity);
    obs_data_set_default_double(data,"nr_pass2_local_structure",tuning.localStructure);
    obs_data_set_default_double(data,"nr_pass2_local_tone",0);
    obs_data_set_default_double(data,"nr_pass2_skin_structure",tuning.skinStructure);
    obs_data_set_default_bool(data,"nr_pass2_auto_mask",tuning.autoMask);
    obs_data_set_default_double(data,"nr_temporal_stability",0.80);
    obs_data_set_default_bool(data,"nr_hold_enabled",true);
    obs_data_set_default_double(data,"nr_hold_strength",0.80);
    obs_data_set_default_double(data,"nr_hold_tolerance",0.020);
    obs_data_set_default_int(data,"nr_motion_mode",0);
    obs_data_set_default_int(data,"nr_quick_preset",0);
    obs_data_set_default_int(data,"nr_fps_overlay",0);
}
static bool within(double value,double low,double high)
{
    return std::isfinite(value) && value>=low && value<=high;
}
static ModelTuning read_tuning_keys(obs_data_t* data,const char* prefix,bool& valid)
{
    const auto k=[prefix](const char* s){return std::string(prefix)+s;};
    const auto preset=obs_data_get_int(data,k("preset").c_str());
    const auto style=obs_data_get_int(data,k("style").c_str());
    const double intensity=obs_data_get_double(data,k("intensity").c_str());
    const double localStructure=obs_data_get_double(data,k("local_structure").c_str());
    const double localTone=obs_data_get_double(data,k("local_tone").c_str());
    const double skinStructure=obs_data_get_double(data,k("skin_structure").c_str());
    const bool presetValid=preset>=0 && preset<=3, styleValid=style>=0 && style<=2;
    valid=presetValid && styleValid && within(intensity,0,2) && within(localStructure,0,2) &&
        within(localTone,0,2) && within(skinStructure,-1,2);
    return {presetValid?static_cast<uint32_t>(preset):0u,styleValid?static_cast<uint32_t>(style):0u,
        static_cast<float>(intensity),static_cast<float>(localStructure),static_cast<float>(localTone),
        static_cast<float>(skinStructure),obs_data_get_bool(data,k("auto_mask").c_str())};
}
static Values read(obs_data_t* data)
{
    bool tuningValid=false;
    const auto tuning=read_tuning_keys(data,"nr_",tuningValid);
    Values v{obs_data_get_bool(data,"nr_enabled"),obs_data_get_string(data,"nr_runtime_path"),
        {static_cast<float>(obs_data_get_double(data,"nr_strength"))},{},tuning};
    const bool pass2Custom=obs_data_get_bool(data,"nr_pass2_custom");
    bool pass2Valid=true;
    if(pass2Custom) {
        v.pass2Tuning.emplace(read_tuning_keys(data,"nr_pass2_",pass2Valid));
    }
    const double strength=obs_data_get_double(data,"nr_strength");
    const auto passes=obs_data_has_user_value(data,"nr_passes") || obs_data_has_default_value(data,"nr_passes")?
        obs_data_get_int(data,"nr_passes"):1;
    const auto scale=obs_data_has_user_value(data,"nr_work_scale") || obs_data_has_default_value(data,"nr_work_scale")?
        obs_data_get_int(data,"nr_work_scale"):100;
    const bool passesValid=passes>=1 && passes<=2, scaleValid=scale>=50 && scale<=100;
    if(passesValid)v.passes=static_cast<uint32_t>(passes);
    if(scaleValid)v.workScale=static_cast<uint32_t>(scale);
    const double temporal=obs_data_has_user_value(data,"nr_temporal_stability") || obs_data_has_default_value(data,"nr_temporal_stability")?
        obs_data_get_double(data,"nr_temporal_stability"):0.0;
    const bool holdOn=obs_data_get_bool(data,"nr_hold_enabled");
    const double holdStrength=obs_data_has_user_value(data,"nr_hold_strength") || obs_data_has_default_value(data,"nr_hold_strength")?
        obs_data_get_double(data,"nr_hold_strength"):0.80;
    const double holdTolerance=obs_data_has_user_value(data,"nr_hold_tolerance") || obs_data_has_default_value(data,"nr_hold_tolerance")?
        obs_data_get_double(data,"nr_hold_tolerance"):0.020;
    const auto motionMode=obs_data_has_user_value(data,"nr_motion_mode") || obs_data_has_default_value(data,"nr_motion_mode")?
        obs_data_get_int(data,"nr_motion_mode"):0;
    const bool stabValid=within(temporal,0,1) && within(holdStrength,0.1,1) && within(holdTolerance,0.005,0.10) &&
        motionMode>=0 && motionMode<=2;
    if(stabValid) {
        v.stabilize={static_cast<float>(temporal),holdOn?static_cast<float>(holdStrength):0.0f,
            static_cast<float>(holdTolerance),static_cast<uint32_t>(motionMode)};
    }
    if(!std::isfinite(strength) || strength<0 || strength>1)v.reason="invalidStrength";
    else if(!tuningValid || !pass2Valid || !stabValid)v.reason="invalidTuning";
    else if(!passesValid)v.reason="invalidPasses";
    else if(!scaleValid)v.reason="invalidWorkScale";
    return v;
}
Values decode(obs_data_t* data,const std::string& admittedText,const std::string& immutableRuntime)
{
    auto v=read(data);
    if(immutableRuntime.empty()) {if(v.reason.empty())v.reason="filterRecreateRequired";}
    else if(v.runtimePath==admittedText || v.runtimePath==immutableRuntime)v.runtimePath=immutableRuntime;
    else if(v.reason.empty())v.reason="runtimeChangeRequiresRecreate";
    return v;
}
Values admit(obs_data_t* data,RuntimeInspector inspector)
{
    auto v=read(data);
    try {
        const auto path=utf8_path(v.runtimePath);
        if(!path.is_absolute()) {if(v.reason.empty())v.reason="runtimePathNotAbsolute";return v;}
        if(!std::filesystem::is_regular_file(path)) {if(v.reason.empty())v.reason="runtimeMissing";return v;}
        const auto canonical=std::filesystem::canonical(path);
        const auto inspection=inspector({canonical,runtime_sha256});
        if(!inspection.accepted) {if(v.reason.empty())v.reason=inspection.reason;return v;}
        v.runtimePath=canonical.generic_string();
    } catch(const std::exception&) {if(v.reason.empty())v.reason="runtimePathUnsupported";}
    return v;
}
std::shared_ptr<const Settings> updated(const std::shared_ptr<const Settings>& old,const Values& v)
{
    if(old && old->enabled==v.enabled && old->runtimePath==v.runtimePath && old->tuning==v.tuning &&
       old->passes==v.passes && old->workScale==v.workScale && old->pass2Tuning==v.pass2Tuning &&
       old->stabilize==v.stabilize &&
       (old->strength.value==v.strength.value || (std::isnan(old->strength.value) && std::isnan(v.strength.value))))return old;
    return std::make_shared<const Settings>(Settings{{old?old->revision.value+1:1},v.enabled,v.runtimePath,
        v.strength,v.tuning,v.passes,v.workScale,v.pass2Tuning,v.stabilize});
}
bool effective_enabled(bool configured,bool sourceEnabled) {return configured && sourceEnabled;}
StatusSnapshot status(Status state,Reason reason)
{
    StatusSnapshot result;
    switch(state) {
    case Status::Applied:result.state="ready";break;
    case Status::Priming:result.state="priming";break;
    case Status::Disabled:result.state="bypassed";result.reason="disabled";return result;
    case Status::Bypassed:result.state="bypassed";break;
    case Status::Error:result.state="error";break;
    }
    switch(reason) {
    case Reason::None:result.reason="none";break;
    case Reason::MissingIdentity:result.reason="missingIdentity";break;
    case Reason::InvalidSource:result.reason="invalidSource";break;
    case Reason::UnknownValidity:result.reason="unknownValidity";break;
    case Reason::InvalidDescriptor:result.reason="invalidDescriptor";break;
    case Reason::InvalidSettings:result.reason="invalidSettings";break;
    case Reason::OldEpoch:result.reason="oldEpoch";break;
    case Reason::KeyMismatch:result.reason="keyMismatch";break;
    case Reason::MissingPrevious:result.reason="missingPrevious";break;
    case Reason::DeadlineMiss:result.reason="deadlineMiss";break;
    case Reason::Expired:result.reason="expired";break;
    case Reason::Cancelled:result.reason="cancelled";break;
    case Reason::EvaluationFailure:result.reason="evaluationFailure";break;
    case Reason::SlotsFull:result.reason="slotsFull";break;
    case Reason::RetirementBudget:result.reason="retirementBudget";break;
    case Reason::PassFailed:result.reason="passFailed";break;
    case Reason::WorkScaleTooSmall:result.reason="workScaleTooSmall";break;
    }
    return result;
}
void sync_visibility(obs_properties_t* props,obs_data_t* data)
{
    const bool twoPass=obs_data_get_int(data,"nr_passes")>=2;
    const bool pass2Custom=obs_data_get_bool(data,"nr_pass2_custom");
    const bool holdOn=obs_data_get_bool(data,"nr_hold_enabled");
    if(auto* g=obs_properties_get(props,"nr_grp_pass2"))obs_property_set_visible(g,twoPass);
    for(const auto* k:{"nr_pass2_preset","nr_pass2_style","nr_pass2_intensity","nr_pass2_local_structure",
        "nr_pass2_local_tone","nr_pass2_skin_structure","nr_pass2_auto_mask"}) {
        if(auto* p=obs_properties_get(props,k))obs_property_set_visible(p,twoPass && pass2Custom);
    }
    for(const auto* k:{"nr_hold_strength","nr_hold_tolerance"}) {
        if(auto* p=obs_properties_get(props,k))obs_property_set_visible(p,holdOn);
    }
}
static bool on_nr_preset_or_layout_changed(obs_properties_t* props,obs_property_t* prop,obs_data_t* data)
{
    if(!props || !data)return false;
    const char* pname=prop?obs_property_name(prop):nullptr;
    if(pname && std::string(pname)=="nr_quick_preset") {
        const auto preset=obs_data_get_int(data,"nr_quick_preset");
        if(preset>=1 && preset<=4) {
            obs_data_set_double(data,"nr_strength",1.0);
            obs_data_set_int(data,"nr_preset",2);
            obs_data_set_double(data,"nr_local_tone",0.0);
            obs_data_set_double(data,"nr_skin_structure",-1.0);
            obs_data_set_bool(data,"nr_auto_mask",true);
            obs_data_set_double(data,"nr_temporal_stability",0.80);
            obs_data_set_bool(data,"nr_hold_enabled",true);
            obs_data_set_double(data,"nr_hold_strength",0.80);
            obs_data_set_double(data,"nr_hold_tolerance",0.020);
            if(preset==1) {
                obs_data_set_int(data,"nr_passes",1);
                obs_data_set_int(data,"nr_work_scale",100);
                obs_data_set_int(data,"nr_style",0);
                obs_data_set_double(data,"nr_intensity",1.0);
                obs_data_set_double(data,"nr_local_structure",1.0);
                obs_data_set_bool(data,"nr_pass2_custom",false);
            } else if(preset==2) {
                obs_data_set_int(data,"nr_passes",2);
                obs_data_set_int(data,"nr_work_scale",100);
                obs_data_set_int(data,"nr_style",0);
                obs_data_set_double(data,"nr_intensity",1.0);
                obs_data_set_double(data,"nr_local_structure",1.0);
                obs_data_set_bool(data,"nr_pass2_custom",true);
                obs_data_set_int(data,"nr_pass2_preset",2);
                obs_data_set_int(data,"nr_pass2_style",2);
                obs_data_set_double(data,"nr_pass2_intensity",1.0);
                obs_data_set_double(data,"nr_pass2_local_structure",1.0);
                obs_data_set_double(data,"nr_pass2_local_tone",0.0);
                obs_data_set_double(data,"nr_pass2_skin_structure",-1.0);
                obs_data_set_bool(data,"nr_pass2_auto_mask",true);
            } else if(preset==3) {
                obs_data_set_int(data,"nr_passes",1);
                obs_data_set_int(data,"nr_work_scale",100);
                obs_data_set_int(data,"nr_style",1);
                obs_data_set_double(data,"nr_intensity",0.85);
                obs_data_set_double(data,"nr_local_structure",0.85);
                obs_data_set_bool(data,"nr_pass2_custom",false);
            } else if(preset==4) {
                obs_data_set_int(data,"nr_passes",1);
                obs_data_set_int(data,"nr_work_scale",75);
                obs_data_set_int(data,"nr_style",0);
                obs_data_set_double(data,"nr_intensity",1.0);
                obs_data_set_double(data,"nr_local_structure",1.0);
                obs_data_set_bool(data,"nr_pass2_custom",false);
            }
        }
    }
    sync_visibility(props,data);
    return true;
}
obs_properties_t* controls(const StatusSnapshot& snapshot,const Labels& labels)
{
    auto* props=obs_properties_create();
    auto* grpBasic=obs_properties_create();
    obs_properties_add_bool(grpBasic,"nr_enabled",labels.enabled);
    auto* qp=obs_properties_add_list(grpBasic,"nr_quick_preset",labels.quickPreset,OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(qp,labels.quickPresetCustom,0);
    obs_property_list_add_int(qp,labels.quickPresetVeyra,1);
    obs_property_list_add_int(qp,labels.quickPresetCombo,2);
    obs_property_list_add_int(qp,labels.quickPresetNatural,3);
    obs_property_list_add_int(qp,labels.quickPresetFast,4);
    obs_property_set_long_description(qp,labels.hintQuickPreset);
    obs_property_set_modified_callback(qp,on_nr_preset_or_layout_changed);

    obs_properties_add_float_slider(grpBasic,"nr_strength",labels.strength,0,1,.01);
    auto* pPasses=obs_properties_add_int_slider(grpBasic,"nr_passes",labels.passes,1,2,1);
    obs_property_set_modified_callback(pPasses,on_nr_preset_or_layout_changed);
    auto* pScale=obs_properties_add_int_slider(grpBasic,"nr_work_scale",labels.workScale,50,100,5);
    obs_property_int_set_suffix(pScale,"%");
    obs_property_set_long_description(pScale,labels.hintWorkScale);
    obs_properties_add_path(grpBasic,"nr_runtime_path",labels.runtime,OBS_PATH_FILE,"Runtime (*.dll)",nullptr);
    obs_properties_add_group(props,"nr_grp_basic",labels.groupBasic,OBS_GROUP_NORMAL,grpBasic);

    auto* grpPass1=obs_properties_create();
    obs_properties_add_int_slider(grpPass1,"nr_preset",labels.preset,0,3,1);
    auto* style=obs_properties_add_list(grpPass1,"nr_style",labels.style,OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(style,labels.styleStandard,0);
    obs_property_list_add_int(style,labels.styleNatural,1);
    obs_property_list_add_int(style,labels.styleCinematic,2);
    obs_properties_add_float_slider(grpPass1,"nr_intensity",labels.intensity,0,2,.05);
    obs_properties_add_float_slider(grpPass1,"nr_local_structure",labels.localStructure,0,2,.05);
    obs_properties_add_float_slider(grpPass1,"nr_local_tone",labels.localTone,0,2,.05);
    obs_properties_add_float_slider(grpPass1,"nr_skin_structure",labels.skinStructure,-1,2,.05);
    obs_properties_add_bool(grpPass1,"nr_auto_mask",labels.autoMask);
    obs_properties_add_group(props,"nr_grp_pass1",labels.groupPass1,OBS_GROUP_NORMAL,grpPass1);

    auto* grpPass2=obs_properties_create();
    auto* p2Custom=obs_properties_add_bool(grpPass2,"nr_pass2_custom",labels.pass2Custom);
    obs_property_set_long_description(p2Custom,labels.hintPass2);
    obs_property_set_modified_callback(p2Custom,on_nr_preset_or_layout_changed);
    obs_properties_add_int_slider(grpPass2,"nr_pass2_preset",labels.pass2Preset,0,3,1);
    auto* p2style=obs_properties_add_list(grpPass2,"nr_pass2_style",labels.pass2Style,OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p2style,labels.styleStandard,0);
    obs_property_list_add_int(p2style,labels.styleNatural,1);
    obs_property_list_add_int(p2style,labels.styleCinematic,2);
    obs_properties_add_float_slider(grpPass2,"nr_pass2_intensity",labels.pass2Intensity,0,2,.05);
    obs_properties_add_float_slider(grpPass2,"nr_pass2_local_structure",labels.pass2LocalStructure,0,2,.05);
    obs_properties_add_float_slider(grpPass2,"nr_pass2_local_tone",labels.pass2LocalTone,0,2,.05);
    obs_properties_add_float_slider(grpPass2,"nr_pass2_skin_structure",labels.pass2SkinStructure,-1,2,.05);
    obs_properties_add_bool(grpPass2,"nr_pass2_auto_mask",labels.pass2AutoMask);
    obs_properties_add_group(props,"nr_grp_pass2",labels.groupPass2,OBS_GROUP_NORMAL,grpPass2);

    auto* grpStab=obs_properties_create();
    auto* pTemp=obs_properties_add_float_slider(grpStab,"nr_temporal_stability",labels.temporalStability,0,1,.05);
    obs_property_set_long_description(pTemp,labels.hintTemporal);
    auto* pHold=obs_properties_add_bool(grpStab,"nr_hold_enabled",labels.holdEnabled);
    obs_property_set_long_description(pHold,labels.hintHold);
    obs_property_set_modified_callback(pHold,on_nr_preset_or_layout_changed);
    obs_properties_add_float_slider(grpStab,"nr_hold_strength",labels.holdStrength,.1,1,.05);
    obs_properties_add_float_slider(grpStab,"nr_hold_tolerance",labels.holdTolerance,.005,.10,.005);
    auto* pMot=obs_properties_add_list(grpStab,"nr_motion_mode",labels.motionMode,OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(pMot,labels.motionRefined,0);
    obs_property_list_add_int(pMot,labels.motionBilinear,1);
    obs_property_list_add_int(pMot,labels.motionZero,2);
    obs_property_set_long_description(pMot,labels.hintMotion);
    obs_properties_add_group(props,"nr_grp_stabilize",labels.groupStabilize,OBS_GROUP_NORMAL,grpStab);

    auto* grpStatus=obs_properties_create();
    auto* fpsList=obs_properties_add_list(grpStatus,"nr_fps_overlay",labels.fpsOverlay,OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(fpsList,labels.fpsOverlayOff,0);
    obs_property_list_add_int(fpsList,labels.fpsOverlayTopLeft,1);
    obs_property_list_add_int(fpsList,labels.fpsOverlayTopRight,2);
    obs_properties_add_text(grpStatus,"nr_status",labels.statusText?labels.statusText:snapshot.state.c_str(),OBS_TEXT_INFO);
    obs_properties_add_text(grpStatus,"nr_reason",labels.reasonText?labels.reasonText:snapshot.reason.c_str(),OBS_TEXT_INFO);
    obs_property_set_enabled(obs_properties_get(grpStatus,"nr_status"),false);
    obs_property_set_enabled(obs_properties_get(grpStatus,"nr_reason"),false);
    if(labels.deadlineText) {
        auto* deadline=obs_properties_add_text(grpStatus,"nr_deadline",labels.deadlineText,OBS_TEXT_INFO);
        obs_property_set_enabled(deadline,false);
    }
    if(labels.passStatus.requested>1 || labels.passStatus.effective<labels.passStatus.requested) {
        auto* passes=obs_properties_add_text(grpStatus,"nr_passes_status",labels.passesText?labels.passesText:labels.passes,OBS_TEXT_INFO);
        obs_property_set_enabled(passes,false);
    }
    obs_properties_add_group(props,"nr_grp_status",labels.groupStatus,OBS_GROUP_NORMAL,grpStatus);
    return props;
}
}
