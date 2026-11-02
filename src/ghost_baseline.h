// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Task27 ghost-baseline: CPU-only acceptance gate, tallies, defects and receipt writer.
// Nothing here opens a freeze file for writing; the accepted Task26 calibration is read-only input.
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "ghost_freeze.h"

namespace nr::ghost::baseline {
std::string sha256_hex(std::span<const uint8_t> bytes);
inline std::string sha256_hex(std::string_view text) {
    return sha256_hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),text.size()));
}

// Minimal JSON reader for the exact documents ghost::write_calibration/write_controls emit.
struct Json {
    enum class Kind { Null,Bool,Number,String,Array,Object } kind=Kind::Null;
    bool boolean=false;
    double number=0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string,Json>> members;
    const Json* find(std::string_view key) const;
};
Json parse_json(std::string_view text); // throws std::invalid_argument

struct LaneThresholds {
    double formatBound=0,rawMaximum=0,guideMismatch=0,absoluteVector=0,relativeVector=0;
    double epsilon() const {return std::max(formatBound,rawMaximum);}
};
struct AcceptedCalibration {
    std::array<LaneThresholds,8> lanes{}; // rate_index*4+mode, as ghost::write_calibration orders them.
    std::string calibrationSha256,controlsSha256,hostSha256,hostSourceSha256;
};
// Holdout admission. Reads no file and writes nothing. Every refusal throws std::invalid_argument:
//  * either late-bound hash argument absent/malformed, or not equal to the SHA-256 of the supplied bytes;
//  * the compiled/loaded policy differs from the compiled Task26 freeze constants (ghost::validate_freeze);
//  * the measured document is not the physical, measured, raw-only-frozen Task26 calibration for this policy;
//  * the controls document is not a measured physical document with all nine planted controls rejected.
AcceptedCalibration accept_calibration(std::string_view calibrationJson,std::string_view controlsJson,
    std::string_view acceptedCalibrationSha256,std::string_view acceptedControlsSha256,const FreezePolicy& policy);

// Freeze documents are immutable inputs. These names (and anything under a policy root) are never writable here.
bool frozen_file_name(std::string_view filename);
void require_baseline_output(const std::filesystem::path& file); // throws when the name is a freeze/Task26 result file
std::ofstream open_output(const std::filesystem::path& file);    // checked, binary, truncating

enum class Rung : unsigned { Raw,Binary,UnprotectedNr };
const char* rung_name(Rung);

struct TemplateFailure { unsigned tick=0,prior=0,region=0; double incremental=0,raw=0,total=0,bound=0; };
struct TemplateStat {
    uint64_t scored=0,failures=0;
    unsigned firstFailureTick=0,longest=0,current=0;
    double maxIncremental=0,maxRaw=0,maxTotal=0,boundAtMax=0;
};
struct Defect {
    std::string id,rung,category,detail;
    unsigned rateIndex=0,mode=0,tick=0,prior=0,region=0,persistence=0;
    uint64_t occurrences=0;
    double measured=0,bound=0;
    bool forTask28=false;
};

struct Tally {
    Rung rung=Rung::Raw;
    unsigned rateIndex=0,mode=0;
    double epsilon=0;
    // Cadence/keys (one row per tick of the 256-tick episode).
    uint64_t ticks=0,keyAnomalies=0;
    std::array<uint64_t,4> kinds{}; // OutputKind: raw is CurrentSource (0); processed rungs are 1..3.
    uint64_t firstIngress=0,lastIngress=0,instance=0;
    uint64_t offCurrentSamples=0,lastOffTick=0;
    // Temporal trail (every distinct prior template, both regions, never averaged).
    uint64_t templates=0,zeroContrast=0,templateFailures=0,failedTicks=0;
    double maxIncremental=0,maxRaw=0,maxTotal=0,maxRatio=0;
    TemplateFailure worst{};
    std::vector<TemplateFailure> failures; // first kListedFailures only; TemplateStat keeps the full picture
    std::vector<TemplateStat> stats=std::vector<TemplateStat>(512);
    unsigned longestPersistence=0;
    // Raw transport (OFF/current raw): per-tick scene identity and max per-channel error against epsilon.
    uint64_t rawChecks=0,rawFailures=0;
    double maxRawError=0;
    // Signed 1px channel contrast loss against 2*epsilon.
    uint64_t edgeChecks=0,edgeFailures=0;
    double maxEdgeLoss=0;
    unsigned firstEdgeFailureTick=0;
    // Edit energy against D recorded at the same JobKey.
    uint64_t editSamples=0,editValid=0,editFailures=0,staticPositive=0,movingPositive=0;
    double minValidRetention=std::numeric_limits<double>::infinity(),maxEditRms=0;
    unsigned firstEditFailureTick=0;
    // Codec and guide evidence (lane level).
    bool codecRequired=false;
    uint64_t codecFrames=0,codecErrors=0;
    uint64_t nativeGuides=0,guidePoints=0,modelSamples=0;
    double guideMismatch=0,absoluteVector=0,relativeVector=0;
    LaneThresholds guideBound{};
    bool gateRaw=false,gateEdit=false,gateGuides=false;
    static constexpr size_t kListedFailures=32;

    Tally()=default;
    Tally(Rung r,unsigned rate,unsigned m,double eps):rung(r),rateIndex(rate),mode(m),epsilon(eps) {
        gateRaw=r==Rung::Raw;
        gateEdit=r==Rung::UnprotectedNr;
        gateGuides=r==Rung::UnprotectedNr;
        codecRequired=true;
    }
    void frame(unsigned kind,uint64_t ingress,uint64_t selected,uint64_t instanceId);
    // The raw rung admits an independently observed disabled current output, never an NR selection.
    void off_frame(const OffObservation&,double maximumError);
    // gate==Raw judges the raw trail (X-C) against the bound; otherwise the incremental trail (Y-X) is judged.
    void temporal(unsigned tick,const TemporalReport&);
    void raw_transport(unsigned tick,double maximumError,bool sceneIdentity);
    void edge(unsigned tick,double maximumLoss,uint64_t checks);
    void edit(unsigned tick,bool moving,bool referenceValid,double rms,double retained);
    bool complete() const {return ticks==Fixture::ticks&&!keyAnomalies;}
    bool pass() const;
};

struct Inherited {
    unsigned mode=0;
    // PLAN:91 has 256 episode ticks. Attribution needs a predecessor, so only 1..255 are scored.
    uint64_t ticks=0,templates=0;
    double maxIncremental=0,maxRaw=0,maxTotal=0;
    bool rawEqualsTotal=true,pluginAttributed=false;
};
Inherited attribute_inherited(const Fixture&,const Calibration&);

struct Summary {
    std::string hostSha256,hostSourceSha256;
    FreezePolicy policy;
    AcceptedCalibration accepted;
    std::vector<Tally> tallies;
    std::array<PhysicalControl,9> controls{};
    uint64_t anchors=0;
    std::vector<Inherited> inherited;
    std::vector<std::string> faults; // fault labels; a fault is never a quality result
};
bool controls_valid(const Summary&);
bool rung_pass(const Summary&,Rung);   // all eight lanes present, complete and passing
bool baseline_valid(const Summary&);   // raw and binary pass, controls rejected, no fault, 24 tallies
bool nr_pass(const Summary&);
std::vector<Defect> collect_defects(const Summary&);
const char* result_label(const Summary&);
void write_summary(std::ostream&,const Summary&);

// Worst signed 1px channel contrast loss max(|c|-signed y) over the supplied edges; ghost::detail() fails above 2*epsilon.
double edge_loss(const Image& clean,const Image& output,std::span<const Edge> edges);

// CPU-side admission used by the planted-control tests: scene identity, incremental trail and edge detail.
struct Admission {
    bool identity=false,trail=false,detail=false;
    bool pass() const {return identity&&trail&&detail;}
};
Admission admit_sample(const Fixture&,unsigned expectedTick,const Image& raw,const Image& output,const Calibration&);
struct PlantedControl { const char* name; Admission admission; };
// raw, binary (n-1 delayed copy), then the five planted bad outputs. tick must have three distinct prior contents.
std::array<PlantedControl,7> plant_controls(const Fixture&,unsigned tick,const Calibration&);
std::optional<unsigned> plantable_tick(const Fixture&);
}
