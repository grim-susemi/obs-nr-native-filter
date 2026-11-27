// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace nr::fg::vfg {
// Per-draw answer of the SR filter's `sr_flow_snapshot` handler, reduced to the
// facts the FG tap may act on. `temporal` says the SR presented a frame it
// captured on an earlier tick; `upscaled` says that presented frame is at a
// different extent than the source. Extent alone is never a delay signal:
// bilinear 1080p->1440p changes extent without delaying, and DLSS at 1:1
// (DLAA) delays without changing extent.
struct SrPresentSignal {
    bool answered = false;          // the SR filter's handler answered for this draw
    bool temporal = false;          // the presented colour is an earlier capture
    bool upscaled = false;          // the presented colour differs in extent
    std::uint64_t presentTick = 0;  // raw OBS frame time of the presented colour
    std::uint64_t capturedTick = 0; // raw OBS frame time captured by this draw
};

// Which signal produced the decision. Snapshot is authoritative; the extent
// comparison is the documented fallback kept for a chain with no SR telemetry.
enum class SrDelayPath : std::uint8_t { Snapshot, ExtentFallback };

struct SrDisplayDecision {
    std::uint64_t displayTick = 0; // canonical OBS tick whose colour this publish carries
    std::uint64_t shift = 0;       // canonical ticks the colour trails this capture
    SrDelayPath path = SrDelayPath::ExtentFallback;
};

// The capture this tap is about to publish. `extentChanged` is the old proxy,
// kept only for the fallback path.
struct SrDraw {
    std::uint64_t captureTickNs = 0; // obs_get_video_frame_time() for this draw
    std::uint64_t intervalNs = 0;    // obs_get_frame_interval_ns()
    bool extentChanged = false;      // output extent != target base extent
};

// OBS's canonical video tick token (the identity the flow broker keys display
// records by), duplicated here so the FG test lanes need no libobs.
inline std::uint64_t sr_canonical_tick(std::uint64_t time, std::uint64_t interval)
{
    return time && interval ? time / interval + 1 : 0;
}

// The snapshot is authoritative whenever the SR answers: a temporal draw whose
// telemetry was written for this exact capture tick displays the tick it
// presented; every other SR draw (spatial fallback, bilinear resize, or
// telemetry that did not advance for this capture) displays this tick. Only a
// chain with no SR telemetry at all takes the extent fallback; the caller logs
// the chosen path so the fallback is never silent.
inline SrDisplayDecision sr_display_decision(const SrDraw& draw, const SrPresentSignal& signal)
{
    const auto canonical = sr_canonical_tick(draw.captureTickNs, draw.intervalNs);
    const bool delayed = signal.answered
        ? signal.temporal && signal.capturedTick && signal.capturedTick == draw.captureTickNs
        : draw.extentChanged;
    auto display = canonical;
    if (delayed) {
        const auto presented = signal.answered ? sr_canonical_tick(signal.presentTick, draw.intervalNs) : 0;
        display = presented && presented < canonical ? presented : (canonical > 1 ? canonical - 1 : canonical);
    }
    return {display, canonical - display, signal.answered ? SrDelayPath::Snapshot : SrDelayPath::ExtentFallback};
}
} // namespace nr::fg::vfg
