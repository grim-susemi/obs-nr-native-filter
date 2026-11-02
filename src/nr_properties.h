// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_runtime.h"
#include <obs.h>
namespace nr::properties {
inline constexpr char default_runtime[]="C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll";
inline constexpr char runtime_sha256[]="e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a";
struct Values { bool enabled;std::string runtimePath;CompositionStrength strength;std::string reason; };
struct StatusSnapshot {std::string state,reason;};
struct Configuration {std::shared_ptr<const Settings> settings;std::string reason;};
void defaults(obs_data_t*);
std::filesystem::path utf8_path(const std::string&);
Values decode(obs_data_t*,const std::string& admittedText,const std::string& immutableRuntime);
using RuntimeInspector=runtime::Inspection (*)(const runtime::RuntimeIdentity&);
Values admit(obs_data_t*,RuntimeInspector=runtime::inspect);
std::shared_ptr<const Settings> updated(const std::shared_ptr<const Settings>&,const Values&);
bool effective_enabled(bool configured,bool sourceEnabled);
StatusSnapshot status(Status,Reason);
obs_properties_t* controls(const StatusSnapshot&,const char* enabled="Enable native NR",
    const char* runtime="NR runtime file",const char* strength="Composition strength");
}
