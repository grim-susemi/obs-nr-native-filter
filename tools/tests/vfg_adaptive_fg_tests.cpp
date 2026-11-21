// SPDX-License-Identifier: GPL-3.0-only
#include "../fg-presenter/vfg_adaptive_fg.h"
#include "../fg-presenter/vfg_cadence.h"
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
    expect(plan_cadence_adaptive(1'000'000'000, 0, 8'333'332, 3).multiplier == 0 &&
        plan_cadence_adaptive(1'000'000'000, 0, 8'333'333, 3).multiplier == 3 &&
        plan_cadence_adaptive(1'000'000'000, 0, 50'000'000, 4).multiplier == 4 &&
        plan_cadence_adaptive(1'000'000'000, 0, 50'000'001, 4).multiplier == 0 &&
        plan_cadence_adaptive(1'000'000'000, 0, 5'000'000, 4).multiplier == 0 &&
        plan_cadence_adaptive(1'000'000'000, 0, 100'000'000, 4).multiplier == 0,
        "period bounds are inclusive at 8.333333-50 ms");
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
    monitor_query();
    std::printf("%s vfg_adaptive_fg (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
