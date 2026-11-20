// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_runtime.h"
#include "nr_model_passes.h"
#include <obs.h>
namespace nr::properties {
inline constexpr char default_runtime[]="";
inline constexpr char runtime_sha256[]="e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a";
// tuning is decoded from the seven nr_* model controls; ranges follow OptiScaler [DlssNr]
// (preset 0..3, style 0..2, intensity/localStructure/localTone 0..2, skinStructure -1..2).
struct Values { bool enabled;std::string runtimePath;CompositionStrength strength;std::string reason;ModelTuning tuning;
    uint32_t passes=1,workScale=100;std::optional<ModelTuning> pass2Tuning{};StabilizeSettings stabilize{}; };
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
Values admit(obs_data_t*,RuntimeInspector=runtime::inspect,const std::filesystem::path& installedRuntime={});
std::shared_ptr<const Settings> updated(const std::shared_ptr<const Settings>&,const Values&);
bool effective_enabled(bool configured,bool sourceEnabled);
StatusSnapshot status(Status,Reason);
struct Labels {
    const char* enabled="Enable native NR";
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
    const char* pass2Custom="Custom 2nd pass tuning";
    const char* pass2Preset="2nd pass preset (0-3)";
    const char* pass2Style="2nd pass style";
    const char* pass2Intensity="2nd pass intensity (0-2)";
    const char* pass2LocalStructure="2nd pass local structure (0-2)";
    const char* pass2LocalTone="2nd pass local tone (0-2)";
    const char* pass2SkinStructure="2nd pass skin structure (-1 = follow local)";
    const char* pass2AutoMask="2nd pass automatic mask";
    const char* groupBasic="Basic / Preset / Performance";
    const char* groupPass1="1st Pass NR Picture Control";
    const char* groupPass2="2nd Pass Combo (2-pass only)";
    const char* groupStabilize="Anti-flicker / Stabilizer (Veyra)";
    const char* groupStatus="FPS HUD / Status";
    const char* quickPreset="Quick preset";
    const char* quickPresetCustom="Custom (keep current values)";
    const char* quickPresetVeyra="Veyra Standard + Anti-flicker (Recommended)";
    const char* quickPresetCombo="2-Pass Combo (Standard + Cinematic)";
    const char* quickPresetNatural="Natural (Soft detail)";
    const char* quickPresetFast="Fast (75% working scale)";
    const char* temporalStability="Temporal stability (0-1)";
    const char* holdEnabled="Static output stabilizer (zero-ghosting)";
    const char* holdStrength="Static stabilizer strength (0.1-1)";
    const char* holdTolerance="Static change tolerance (0.005-0.10)";
    const char* motionMode="NR motion tracking mode";
    const char* motionRefined="Refined optical flow (NvofDensify, anti-wobble)";
    const char* motionBilinear="Standard optical flow";
    const char* motionZero="Zero motion (disable motion warp)";
    const char* hintMotion="Cost-gated 3x3 refined optical flow stops 4x4 block wobble around moving edges and static UI.";
    const char* fpsOverlay="Pre/Post FPS overlay";
    const char* fpsOverlayOff="Off (clean output)";
    const char* fpsOverlayTopLeft="Top-Left";
    const char* fpsOverlayTopRight="Top-Right";
    const char* hintQuickPreset="Apply a Veyra-style recommended profile in one click, or leave on Custom.";
    const char* hintWorkScale="Lower working scale (75%) saves GPU frame time on 1440p/4K or 2-pass.";
    const char* hintPass2="Run a second NR pass with an independent style (e.g. 1st Standard + 2nd Cinematic).";
    const char* hintTemporal="Motion-compensated temporal smoothing using NVOF optical flow (reduces camera-pan shimmer).";
    const char* hintHold="Locks stationary background/UI pixels when source luma changes by <= tolerance; zero ghosting on moving objects.";
    const char* statusText=nullptr; // already localized status/reason line; null = raw code
    const char* reasonText=nullptr;
    const char* deadlineText=nullptr;
    const char* passesText=nullptr;
    model::PassStatus passStatus{1,1,0,0};
};
void sync_visibility(obs_properties_t*,obs_data_t*);
obs_properties_t* controls(const StatusSnapshot&,const Labels& labels=Labels{});
}
