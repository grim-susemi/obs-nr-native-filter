// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_runtime.h"
#include <obs.h>
namespace nr::properties {
inline constexpr char default_runtime[]="C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll";
inline constexpr char runtime_sha256[]="e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a";
// tuning is decoded from the seven nr_* model controls; ranges follow OptiScaler [DlssNr]
// (preset 0..3, style 0..2, intensity/localStructure/localTone 0..2, skinStructure -1..2).
struct Values { bool enabled;std::string runtimePath;CompositionStrength strength;std::string reason;ModelTuning tuning;
    uint32_t passes=1,workScale=100; };
struct StatusSnapshot {std::string state,reason;};
std::optional<unsigned> deadline_warning(uint32_t total,uint32_t late);
struct DeadlineSnapshot {const uint64_t packed;const std::optional<unsigned> warning;const bool changed;};
// One graphics-thread owner per filter; caller supplies each distinct nonzero video timestamp.
class DeadlineWindow {
    uint64_t start_=0;
    uint32_t total_=0,late_=0;
    bool warning_=false;
public:
    std::optional<DeadlineSnapshot> observe(uint64_t now,const StatusSnapshot&);
};
struct Configuration {std::shared_ptr<const Settings> settings;std::string reason;};
void defaults(obs_data_t*);
std::filesystem::path utf8_path(const std::string&);
Values decode(obs_data_t*,const std::string& admittedText,const std::string& immutableRuntime);
using RuntimeInspector=runtime::Inspection (*)(const runtime::RuntimeIdentity&);
Values admit(obs_data_t*,RuntimeInspector=runtime::inspect);
std::shared_ptr<const Settings> updated(const std::shared_ptr<const Settings>&,const Values&);
bool effective_enabled(bool configured,bool sourceEnabled);
StatusSnapshot status(Status,Reason);
struct Labels {
    const char* enabled="Enable native NR";
    const char* runtime="NR runtime file";
    const char* strength="Composition strength";
    const char* passes="NR passes";
    const char* workScale="NR working scale";
    const char* preset="NR model preset (0-3)";
    const char* style="NR style";
    const char* styleStandard="Standard";
    const char* styleNatural="Natural";
    const char* styleCinematic="Cinematic";
    const char* intensity="NR intensity (0-2)";
    const char* localStructure="Local structure (0-2)";
    const char* localTone="Local tone (0-2)";
    const char* skinStructure="Skin structure (-1 = follow local structure)";
    const char* autoMask="Automatic mask";
    const char* statusText=nullptr; // already localized status/reason line; null = raw code
    const char* reasonText=nullptr;
    const char* deadlineText=nullptr;
};
obs_properties_t* controls(const StatusSnapshot&,const Labels& labels=Labels{});
}
