// SPDX-License-Identifier: GPL-3.0-only
#include "nr_properties.h"
#include "nr_core.h"
namespace nr::properties {
std::filesystem::path utf8_path(const std::string& value)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));
}
void defaults(obs_data_t* data)
{
    obs_data_set_default_bool(data,"nr_enabled",true);
    obs_data_set_default_string(data,"nr_runtime_path",default_runtime);
    obs_data_set_default_double(data,"nr_strength",1);
}
static Values read(obs_data_t* data)
{
    Values v{obs_data_get_bool(data,"nr_enabled"),obs_data_get_string(data,"nr_runtime_path"),
        {static_cast<float>(obs_data_get_double(data,"nr_strength"))},{}};
    const double strength=obs_data_get_double(data,"nr_strength");
    if(!std::isfinite(strength) || strength<0 || strength>1)v.reason="invalidStrength";
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
    if(old && old->enabled==v.enabled && old->runtimePath==v.runtimePath &&
       (old->strength.value==v.strength.value || (std::isnan(old->strength.value) && std::isnan(v.strength.value))))return old;
    return std::make_shared<const Settings>(Settings{{old?old->revision.value+1:1},v.enabled,v.runtimePath,
        v.strength,old?old->tuning:core_default_tuning()});
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
    }
    return result;
}
obs_properties_t* controls(const StatusSnapshot& snapshot,const char* enabled,const char* runtime,const char* strength)
{
    auto* props=obs_properties_create();
    obs_properties_add_bool(props,"nr_enabled",enabled);
    obs_properties_add_path(props,"nr_runtime_path",runtime,OBS_PATH_FILE,"Runtime (*.dll)",nullptr);
    obs_properties_add_float_slider(props,"nr_strength",strength,0,1,.01);
    obs_properties_add_text(props,"nr_status",snapshot.state.c_str(),OBS_TEXT_INFO);
    obs_properties_add_text(props,"nr_reason",snapshot.reason.c_str(),OBS_TEXT_INFO);
    obs_property_set_enabled(obs_properties_get(props,"nr_status"),false);
    obs_property_set_enabled(obs_properties_get(props,"nr_reason"),false);
    return props;
}
}
