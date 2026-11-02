// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include "ghost_oracle.h"

namespace nr::ghost {
inline constexpr std::array<const char*,5> freeze_files{
    "calibration.json","holdout.json","masks.json","epsilon.json","thresholds.json"};
inline constexpr std::array<const char*,5> freeze_hashes{
    "083846ff95bc9bd819cd2d0424d98acd86554ca6023a6296e1b7acc200f6f881",
    "d15c5110cc727142e290b5d7723eb8abaad08fd2c61ea4b638ca6c8cc681c2ac",
    "3a0bbba94fb75f18e5d77f61f6469809e4d4af61ed46549dab46b37cd99dd16f",
    "2562e4187b776da35d5fb84ca384b2b7fbcad81fd5a86c3e4fd5fe8bd1730411",
    "60c0504d36ed3f4687584f72f1c8292525cae7e52f265c131aad8b1a86cb6e61"};
// SHA-256 of the sealed project/nr_ghost_scenario.cpp row compiled into the host; written into receipts as host_source_sha256.
// The digest of that row cannot depend on this header (the host only names the constant). Any build whose compiled
// nr_ghost_scenario.cpp bytes differ from this value is a closure divergence (see FIX-NOTES.md).
inline constexpr char compiled_host_source_sha256[]="124bae6d4124bdf945591eac1ceb4e447186f7a84c05755e9b7f0fef8301f286";
struct FreezePolicy {
    uint32_t calibrationSeed=0,holdoutSeed=0,ticks=0;
    std::array<uint32_t,2> rateNumerators{},rateDenominators{};
    uint32_t width=0,height=0,visibleTop=0,physicalWidth=0,physicalHeight=0;
    double editRetention=0;
    double temporalFactor=0,detailFactor=0,editFactor=0;
    std::array<uint32_t,4> staticEdit{};
    uint32_t movingInset=0;
    std::array<std::string,5> hashes;
    std::string manifestHash;
};
// These are the existing oracle's numeric semantics, not parsed prose.
inline constexpr double temporal_epsilon_factor=2,detail_epsilon_factor=2,edit_epsilon_factor=8;
void validate_freeze(const FreezePolicy&);
struct PhysicalLane {
    uint64_t rawSamples=0,guideSamples=0,guidePoints=0,modelSamples=0,codecFrames=0,codecErrors=0;
    uint64_t offCurrentSamples=0,offInstance=0,offIngress=0,offTick=0;
    std::optional<double> formatBound,rawMaximum,guideMismatch,absoluteVector,relativeVector;
    bool closed=false;
};
struct OffObservation {
    const Fixture& fixture;
    unsigned sourceTick;
    FrameKey currentKey,outputKey;
    uint64_t currentTick,outputTick,expectedTick,settingsRevision;
    bool configurationEnabled,filterEnabled;
    OutputKind kind;
    PixelView current,output;
    std::span<const uint8_t> currentBytes,outputBytes;
    unsigned bytesPerPixel;
    double epsilon;
};
// Count only a genuinely disabled output, not a raw/current or delayed-copy row.
void collect_off(PhysicalLane&,const OffObservation&);
bool measured(const PhysicalLane&);
void require_calibration(const std::array<PhysicalLane,8>&);
struct PhysicalControl {
    uint64_t cases=0;
    std::optional<bool> rejected;
    bool actualPixels=false;
};
void require_controls(const std::array<PhysicalControl,9>&,uint64_t positiveAnchors);
struct ControlInput {
    const Fixture& fixture;
    unsigned tick;
    const Image& current;
    const Image& previous;
    const Image& guideCurrent;
    const Image& guidePrevious;
    Calibration calibration;
    FrameKey currentKey,selectedKey;
    JobKey completedKey,expectedCompleted,guideKey,referenceKey;
    uint64_t graphicsMask;
    bool physicalPixels;
};
std::array<PhysicalControl,9> calibration_controls(const ControlInput&);
inline constexpr std::array<const char*,9> physical_control_names{
    "blend_2percent","blur_1pixel","frozen","extra_delay","dropped","stale_guide",
    "relabeled_stale_guide","reordered_completion","poststamp_only"};
// A missing observation stays null. These writers never substitute CPU values.
void write_calibration(std::ostream&,const FreezePolicy&,const std::array<PhysicalLane,8>&,
                       const std::string& hostHash,const std::string& sourceHash);
void write_controls(std::ostream&,const FreezePolicy&,const std::array<PhysicalControl,9>&,
                    uint64_t positiveAnchors,const std::string& hostHash,const std::string& sourceHash);
}
