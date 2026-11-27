// SPDX-License-Identifier: GPL-3.0-only
#include "../fg-presenter/vfg_adaptive_fg.h"
#include "../fg-presenter/vfg_cadence.h"
#include "../fg-presenter/vfg_live_anchor.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

using namespace nr::fg::vfg;
namespace {
unsigned failures{};
void expect(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
void selection_matrix()
{
    constexpr double targets[]{120, 144, 240};
    constexpr double rates[]{30, 40, 60, 90};
    constexpr uint32_t expected[3][4]{{4, 3, 2, 2}, {4, 4, 2, 2}, {4, 4, 4, 3}};
    for (size_t t = 0; t < 3; ++t)
        for (size_t r = 0; r < 4; ++r)
            expect(adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, rates[r], targets[t]) == expected[t][r],
                "ceiling-four selection matrix");
    const double content = 60;
    const double target = content * std::sqrt(2.0 * 3.0);
    expect(adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, content, target) == 2, "exact log-distance tie picks lower multiplier");
    expect(adaptive_multiplier(AdaptiveBackend::PushFlow, 4, 30, 120) == 4 &&
        adaptive_multiplier(AdaptiveBackend::AnrExperimental, 3, 30, 120) == 3, "backend selectable ranges and ceiling");
    expect(adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, 34.5, 120) == 4,
        "34.5 FPS selector retains the log-distance choice");
    expect(adaptive_multiplier(AdaptiveBackend::NativeVfg, 1, 30, 120) == 1 &&
        adaptive_multiplier(AdaptiveBackend::NativeVfg, 2, 30, 120) == 2, "low ceiling bypass");
    expect(!selectable_multiplier(AdaptiveBackend::XeFg, 1) && selectable_multiplier(AdaptiveBackend::XeFg, 2) &&
        !selectable_multiplier(AdaptiveBackend::XeFg, 3) && !selectable_multiplier(AdaptiveBackend::XeFg, 4),
        "official XeFG exposes only the documented 2x multiplier");
    expect(!selectable_multiplier(AdaptiveBackend::XeMfg, 1) && selectable_multiplier(AdaptiveBackend::XeMfg, 2) &&
        selectable_multiplier(AdaptiveBackend::XeMfg, 3) && selectable_multiplier(AdaptiveBackend::XeMfg, 4) &&
        !selectable_multiplier(AdaptiveBackend::XeMfg, 5), "opt-in XeMFG keeps the 2..4 ceiling");
    expect(adaptive_multiplier(AdaptiveBackend::XeFg, 4, 60, 120) == 2 &&
        adaptive_multiplier(AdaptiveBackend::XeFg, 4, 30, 120) == 2 &&
        adaptive_multiplier(AdaptiveBackend::XeFg, 4, 30, 240) == 2,
        "XeFG selection collapses to 2x regardless of target");
    expect(adaptive_multiplier(AdaptiveBackend::XeMfg, 4, 30, 120) == 4 &&
        adaptive_multiplier(AdaptiveBackend::XeMfg, 3, 30, 90) == 3 &&
        adaptive_multiplier(AdaptiveBackend::XeFg, 1, 30, 120) == 1,
        "XeMFG reaches its ceiling while a 1x ceiling stays original");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    expect(adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, 0, 120) == 4 &&
        adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, 30, 0) == 4 &&
        adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, 30, -60) == 4 &&
        adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, 30, nan) == 4 &&
        adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, nan, 120) == 4, "invalid rates safely retain ceiling");
}
void hold_hysteresis()
{
    AdaptiveMultiplierGate gate;
    bool changed = false;
    expect(gate.update(4, 0, &changed) == 4 && changed, "first choice applies immediately");
    expect(gate.update(3, 1'000'000, &changed) == 4 && !changed, "candidate waits");
    expect(gate.update(4, 2'000'000, &changed) == 4 && !changed, "canceled spike resets hold");
    expect(gate.update(3, 3'000'000, &changed) == 4, "new candidate starts fresh hold");
    expect(gate.update(3, 7'990'000, &changed) == 4 && !changed, "499 ms rejected");
    expect(gate.update(3, 8'000'000, &changed) == 3 && changed, "500 ms accepted");
}
void tracker()
{
    ContentRateTracker tracker;
    for (uint64_t i = 0; i < 7; ++i) (void)tracker.observe(i * 20'000'000, i * 2, false);
    expect(tracker.size() == 7 && tracker.fps() == 0, "fewer than eight unique samples has no estimate");
    expect(std::abs(tracker.observe(140'000'000, 14, false) - 100) < 0.01,
        "ordinal delta estimates unique content rate");
    const auto sizeBeforeRepeat = tracker.size();
    expect(std::abs(tracker.observe(150'000'000, 14, false) - 100) < 0.01 && tracker.size() == sizeBeforeRepeat,
        "repeat ordinal does not add an observation");
    expect(tracker.observe(900'000'001, 16, false) == 0 && tracker.size() == 1,
        "750 ms rolling window expires across a static gap");
    for (uint64_t i = 2; i <= 8; ++i) (void)tracker.observe(900'000'001 + (i - 1) * 20'000'000, 14 + i * 2, false);
    expect(std::abs(tracker.fps() - 100) < 0.01 && tracker.size() == 8,
        "rate reacquires after aged samples leave the window");
    for (uint64_t i = 9; i < 140; ++i) (void)tracker.observe(900'000'001 + i * 20'000'000, 14 + i * 2, false);
    expect(tracker.size() == 38, "rolling capacity ages samples beyond 750 ms");
    ContentRateTracker capacityTracker;
    for (uint64_t i = 0; i < 140; ++i) (void)capacityTracker.observe(i * 1'000'000, i, false);
    expect(capacityTracker.size() == 128 && std::abs(capacityTracker.fps() - 1000) < 0.1,
        "tracker retains at most 128 observations");
}
void still_hold_resume()
{
    constexpr uint64_t tick = 16'666'667;
    ContentRateTracker shortRepeat;
    for (uint64_t i = 0; i < 8; ++i) (void)shortRepeat.observe(i * tick, i, false);
    const auto shortSize = shortRepeat.size();
    expect(std::abs(shortRepeat.observe(8 * tick, 7, false) - 59.9999988) < 0.001 &&
        shortRepeat.size() == shortSize, "ordinary short repeat preserves the current estimate");
    expect(shortRepeat.observe(9 * tick, 8, false) > 0, "motion resumes after an ordinary repeat");

    ContentRateTracker tracker;
    for (uint64_t i = 0; i < 8; ++i) (void)tracker.observe(i * tick, i, false);
    const double before = tracker.fps();
    bool staticHold = false;
    for (uint64_t i = 8; i < 38; ++i) {
        // ContentDedup first emits reason="hold" at i == 10; latch it until
        // the changed-picture decision reason="new" at i == 38.
        staticHold = staticHold || i >= 10;
        (void)tracker.observe(i * tick, 7, staticHold);
    }
    const double afterResume = tracker.observe(38 * tick, 8, false);
    expect(std::abs(before - 60) < 0.001 && afterResume == 0 && tracker.size() == 1,
        "known 516.7 ms still interval is excluded on resume");
    double reacquired = afterResume;
    for (uint64_t i = 1; i <= 7; ++i)
        reacquired = tracker.observe((38 + i) * tick, 8 + i, false);
    const auto period = static_cast<uint64_t>(std::llround(1'000'000'000.0 / reacquired));
    const auto plan = plan_cadence_adaptive(1'000'000'000, 0, period, 2);
    std::printf("STILL_HOLD_RESUME before=%.9f first=%.9f reacquired=%.9f plan=%u samples=%zu\n",
        before, afterResume, reacquired, plan.multiplier, tracker.size());
    expect(std::abs(reacquired - 60) < 0.001 && tracker.size() == 8,
        "eight unique moving frames reacquire 60 FPS");
    expect(plan.multiplier == 2 && plan.target(1) > 0,
        "reacquired rate gives cadence planner a valid period");
}
void adaptive_cadence()
{
    for (const auto [fps, multiplier] : {std::pair{33.0, 4u}, std::pair{36.0, 3u}}) {
        uint64_t previous = 0;
        for (unsigned i = 0; i < 12; ++i) {
            const uint64_t arrival = 1'000'000'000 + uint64_t(i * 1'000'000'000.0 / fps);
            const auto periodNs = static_cast<uint64_t>(std::llround(1'000'000'000.0 / fps));
            const auto plan = plan_cadence_adaptive(arrival, previous, periodNs, multiplier);
            expect(plan.multiplier == multiplier && plan.target(1) >= arrival &&
                plan.target(multiplier - 1) > plan.target(0) && plan.target(multiplier) == plan.originalNs,
                "adaptive cadence emits valid synthetic slots");
            if (previous) expect(plan.originalNs == previous + plan.periodNs, "adaptive cadence keeps source phase");
            previous = plan.originalNs;
        }
        const auto periodNs = static_cast<uint64_t>(std::llround(1'000'000'000.0 / fps));
        const auto down = plan_cadence_adaptive(previous + 1, previous, periodNs, 2);
        const auto up = plan_cadence_adaptive(down.originalNs, down.originalNs, periodNs, multiplier);
        expect(down.multiplier == 2 && up.multiplier == multiplier &&
            up.originalNs == down.originalNs + down.periodNs,
            "continuity survives multiplier switches");
    }
    const auto boundary = plan_cadence_adaptive(1'000'000'000, 0, 28'985'507, 4);
    expect(boundary.multiplier == 4, "planner honors applied 4x at 34.5 FPS");
    expect(plan_cadence_adaptive(1'000'000'000, 0, 7'499'999, 3).multiplier == 0 &&
        plan_cadence_adaptive(1'000'000'000, 0, 7'500'000, 3).multiplier == 3 &&
        plan_cadence_adaptive(1'000'000'000, 0, 50'000'000, 4).multiplier == 4 &&
        plan_cadence_adaptive(1'000'000'000, 0, 50'000'001, 4).multiplier == 0 &&
        plan_cadence_adaptive(1'000'000'000, 0, 5'000'000, 4).multiplier == 0 &&
        plan_cadence_adaptive(1'000'000'000, 0, 100'000'000, 4).multiplier == 0,
        "period bounds are inclusive at 7.5-50 ms");
}
void high_rate_reacquisition()
{
    ContentRateTracker rate;
    uint64_t pts = 0;
    for (uint64_t frame = 0; frame <= 180; ++frame) {
        pts = frame * 1'000'000'000 / 120;
        (void)rate.observe(pts, frame, false);
        (void)rate.observe(pts + 4'166'667, frame, false); // 240 Hz transport, repeated image
    }
    const auto fps = rate.fps();
    const auto mult = adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, fps, 240);
    std::printf("adaptive content_fps=%.6f multiplier=%u presented_fps=%.6f\n", fps, mult, fps * mult);
    expect(std::abs(fps - 120) < 0.000001 && mult == 2, "120 unique FPS on a 240 Hz display selects exactly 2x");
    expect(rate.observe(pts + 4'166'667, 180, false) == fps,
        "a repeated callback with the same PTS and ordinal is not a new content sample");
    expect(rate.observe(pts + 4'166'667 + 500'000'001, 181, false) == 0 && rate.size() == 1,
        "a half-second callback stall starts a fresh content-rate window");
}
void real_release_on_persistent_reject()
{
    LiveAnchorClock clock;
    const auto at = [&](uint64_t qpc, bool accepted) {
        (void)clock.observe({qpc, qpc}, period_50hz);
        return clock.resolve_fg(accepted);
    };
    unsigned earlyHolds = 0;
    for (uint64_t ms = 0; ms <= 1600; ms += 10)
        earlyHolds += at(ms * 1'000'000, ms == 600).fgHeld; // one success interrupts the first run
    constexpr uint64_t threshold = 1'610'000'000; // first uninterrupted rejection was at 610 ms
    const auto before = at(threshold - 1, false);
    const auto onArrival = clock.observe({threshold, threshold}, period_50hz), release = clock.resolve_fg(false);
    const auto pending = clock.observe({threshold + 10'000'000, threshold + 10'000'000}, period_24hz);
    const auto recovered = clock.resolve_fg(true);
    std::printf("fg_release early_holds=%u before_held=%d held=%d rejected_ns=%llu release_qpc_ns=%llu "
        "real_target_ns=%llu next_held=%d recovery_held=%d recovery_target_ns=%llu\n", earlyHolds, before.fgHeld,
        release.fgHeld, release.realTargetNs - 610'000'000, threshold, release.realTargetNs, pending.fgHeld,
        recovered.fgHeld, recovered.realTargetNs);
    expect(!earlyHolds && !before.fgHeld && before.realTargetNs == threshold - 1 + period_50hz &&
        onArrival.realTargetNs == threshold && release.fgHeld && release.realTargetNs == threshold,
        "real frame bypasses phase at exactly one second, not before");
    expect(pending.fgHeld && pending.realTargetNs == threshold + 10'000'000 && !recovered.fgHeld &&
        recovered.realTargetNs == threshold + 10'000'000 + period_24hz,
        "held originals are immediate until admission restores the current phase");
}
void anchor_discards_invalid_epochs()
{
    constexpr uint64_t end = 1'200'000'000;
    LiveAnchorClock history;
    for (uint64_t t = 0; t <= end; t += period_50hz) {
        (void)history.observe({t, t}, period_50hz);
        (void)history.resolve_fg(false);
    }
    const auto baseline = history.resolve_fg(false);
    expect(baseline.locked && baseline.fgHeld && baseline.snaps == 0, "failure injection begins phase locked and held");
    auto limit = history;
    expect(limit.observe({end + 500'000'000, end + 500'000'000}, period_50hz).snaps == 0,
        "an exactly 500 ms gap on both clocks retains the epoch");
    auto spike = history;
    const auto jitter = spike.observe({end + period_50hz, end + 24'000'000}, period_50hz);
    expect(jitter.callbackJitterMs > 0 && jitter.callbackJitterMs < jitter.callbackMinusAnchorMs &&
        jitter.deadline_extra_ns() > 0 && jitter.deadline_extra_ns() < 1'000'000,
        "a single late callback increases jitter slack gradually instead of adopting the raw phase error");
    struct Cut { const char* name; uint64_t pts, qpc; };
    for (const auto [name, pts, qpc] : {Cut{"qpc-duplicate", end + period_50hz, end},
            {"qpc-backwards", end + period_50hz, end - 1}, {"pts-backwards", end - 1, end + period_50hz},
            {"pts-duplicate", end, end + period_50hz}, {"qpc-stall", end + period_50hz, end + 500'000'001},
            {"both-stalled", end + 500'000'001, end + 500'000'001}, {"phase-step", end + period_50hz, end + 90'000'000}}) {
        auto clock = history;
        const auto cut = clock.observe({pts, qpc}, period_50hz);
        const auto firstReject = clock.resolve_fg(false);
        unsigned mislocks = 0;
        for (uint64_t i = 1; i < 13; ++i)
            mislocks += clock.observe({pts + i * period_50hz, qpc + i * period_50hz}, period_50hz).locked;
        const auto resumed = clock.observe({pts + 260'000'000, qpc + 260'000'000}, period_50hz);
        std::printf("anchor_cut=%s qpc_ns=%llu anchor_ns=%llu snaps=%llu locked=%d fgHeld=%d early_locks=%u "
            "relocked=%d ppm=%.6f\n", name, qpc, cut.anchorNs, cut.snaps, cut.locked, cut.fgHeld, mislocks,
            resumed.locked, resumed.clockDriftPpm);
        expect(cut.snaps == 1 && !cut.locked && !cut.fgHeld && cut.anchorNs == qpc && cut.realTargetNs == qpc + period_50hz &&
            cut.callbackMinusAnchorMs == 0 && cut.callbackJitterMs == 0 && cut.deadline_extra_ns() == 0 &&
            cut.clockDriftPpm == 0 && !firstReject.fgHeld, "discontinuity clears timing and the rejection streak");
        expect(!mislocks && resumed.locked && resumed.snaps == 1 && resumed.clockDriftPpm == 0 &&
            resumed.anchorNs == qpc + 260'000'000, "only the new epoch participates in reacquisition");
    }
}
void monitor_query()
{
    expect(query_monitor_refresh_hz(L"\\\\.\\MissingDisplayForTodo4") == 0,
        "invalid named monitor safely returns zero");
    expect(adaptive_multiplier(AdaptiveBackend::NativeVfg, 4, 30, 1) == 4,
        "one-hertz refresh uses the configured ceiling fallback");
}
}
int main()
{
    selection_matrix();
    hold_hysteresis();
    tracker();
    still_hold_resume();
    adaptive_cadence();
    high_rate_reacquisition();
    real_release_on_persistent_reject();
    anchor_discards_invalid_epochs();
    monitor_query();
    std::printf("%s vfg_adaptive_fg (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
