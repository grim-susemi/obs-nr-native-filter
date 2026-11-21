// SPDX-License-Identifier: GPL-3.0-only
#include "vfg_cadence.h"
#include "vfg_pacing_repair.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace nr::fg::vfg;
namespace {
unsigned checks = 0;
void expect(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

constexpr int64_t t60 = 166'667, t120 = 83'333;

double interval_stddev(const std::vector<int64_t>& times, std::size_t begin)
{
    std::vector<double> intervals;
    for (std::size_t i = begin + 1; i < times.size(); ++i)
        intervals.push_back(static_cast<double>(times[i] - times[i - 1]) / 10'000.0);
    double mean = 0;
    for (const double value : intervals) mean += value;
    mean /= static_cast<double>(intervals.size());
    double variance = 0;
    for (const double value : intervals) variance += (value - mean) * (value - mean);
    return std::sqrt(variance / static_cast<double>(intervals.size()));
}

void pattern_30_in_60()
{
    PushPacingRepair repair;
    UniqueCadence correctedCadence, originalCadence;
    std::vector<int64_t> output;
    int64_t sample = 0;
    unsigned repaired = 0, cadence30Locks = 0;
    uint64_t originalPeriod = 0;
    for (unsigned i = 0; i < 120; ++i) {
        const auto result = repair.apply(sample, sample, t60, false);
        output.push_back(result.pts);
        if (result.repaired) ++repaired;
        cadence30Locks += correctedCadence.observe(static_cast<uint64_t>(result.pts) * 100) == period_30hz;
        originalPeriod = originalCadence.observe(static_cast<uint64_t>(sample) * 100);
        expect(result.shift == result.pts - sample, "shift reports this sample");
        expect(std::abs(result.shift) <= t60, "repair shift stays within one transport");
        sample += (i % 2 ? 3 : 1) * t60;
    }
    expect(repair.stats().repaired >= 100, "1/3 pattern repairs after warmup");
    expect(interval_stddev(output, 10) < 1.0, "1/3 pattern output intervals are even");
    expect(cadence30Locks > 80, "repaired cadence repeatedly locks to 30 Hz");
    expect(originalPeriod == 0,
        "unrepaired 50 ms cadence loses its lock");
    std::printf("1/3 repaired=%llu jitter_ms=%.4f maxLate=%lld maxEarly=%lld cadence=%llu\n",
        static_cast<unsigned long long>(repair.stats().repaired), interval_stddev(output, 10),
        static_cast<long long>(repair.stats().maxLate),
        static_cast<long long>(repair.stats().maxEarly),
        static_cast<unsigned long long>(period_30hz));
}

void pattern_40_in_120_and_even_cadence()
{
    PushPacingRepair repair;
    std::vector<int64_t> output;
    int64_t sample = 0;
    for (unsigned i = 0; i < 120; ++i) {
        output.push_back(repair.apply(sample, sample, t120, false).pts);
        sample += (i % 2 ? 4 : 2) * t120;
    }
    expect(repair.stats().repaired > 0, "2/4 pattern repairs");
    expect(interval_stddev(output, 10) < 1.0, "2/4 pattern becomes 25 ms cadence");
    const double meanInterval = static_cast<double>(output.back() - output[10]) / static_cast<double>(output.size() - 11);
    expect(std::abs(meanInterval - 250'000.0) < 10'000.0,
        "2/4 output mean is 25 ms in 100 ns units");

    PushPacingRepair evenRepair;
    int64_t evenSample = 0, maxShift = 0;
    for (unsigned i = 0; i < 90; ++i) {
        const auto result = evenRepair.apply(evenSample, evenSample, t60, false);
        maxShift = (std::max)(maxShift, std::abs(result.shift));
        evenSample += 2 * t60;
    }
    expect(maxShift < 10'000, "even 2/2 cadence shifts less than 1 ms");

    PushPacingRepair native60;
    for (unsigned i = 0; i < 60; ++i) {
        const auto result = native60.apply(static_cast<int64_t>(i) * t60,
            static_cast<int64_t>(i) * t60, t60, false);
        expect(result.pts == static_cast<int64_t>(i) * t60, "60-in-60 timestamps pass through");
    }
    expect(native60.stats().repaired == 0, "60-in-60 content passes through");
}

void variable_stall_and_boundaries()
{
    PushPacingRepair variable;
    int64_t sample = 0;
    for (unsigned i = 0; i < 150; ++i) {
        const auto result = variable.apply(sample, sample, t60, false);
        expect(result.pts >= sample - t60 && result.pts <= sample + t60,
            "variable-rate output remains bounded");
        constexpr int offsets[]{0, 6, 0, -6, 0, 0, 6, 0, -6, 0, 0, 6, 0, -6, 0, 0};
        const int64_t next = static_cast<int64_t>(i + 1) * (5 * t60 / 2) +
            static_cast<int64_t>(offsets[(i + 1) % std::size(offsets)]) * t60 / 5;
        sample = next;
    }
    expect(variable.stats().variable > 0 && variable.stats().repaired == 0,
        "unexplained variable cadence remains unrepaired");

    PushPacingRepair stall;
    sample = 0;
    int64_t previousOutput = 0;
    for (unsigned i = 0; i < 35; ++i) {
        const auto result = stall.apply(sample, sample, t60, false);
        expect(result.disposition == PushPacingRepair::Disposition::emit &&
            result.pts <= sample + t60 && (!i || result.pts > previousOutput),
            "stall restart preserves strictly increasing bounded output");
        previousOutput = result.pts;
        sample += (i % 2 ? 3 : 1) * t60;
        if (i == 20) sample += 9 * t60;
    }
    expect(stall.stats().restarts > 0, "stall restarts the fit");
    const auto boundary = stall.apply(-5'000'000, -5'000'000, t60, true);
    expect(boundary.pts == -5'000'000, "explicit boundary permits time-base jump");
}

void transport_delta_contract()
{
    TransportIntervalEstimator estimator;
    for (int64_t t = 1'000'000'000; t <= 1'020'000'000; t += 10'000'000)
        expect(estimator.observe(t) == TransportIntervalEstimator::kDefaultNs, "median estimator warmup");
    expect(estimator.observe(1'030'000'000) == 10'000'000, "rolling median uses regular intervals");
    expect(estimator.observe(1'080'000'000) == 10'000'000, "median rejects a long interval outlier");
    for (const int64_t t : std::array<int64_t, 4>{1'070'000'000, 1'060'000'000, 1'070'000'000, INT64_MIN})
        expect(estimator.observe(t) == TransportIntervalEstimator::kDefaultNs, "non-monotonic input resets estimator");
    TransportIntervalEstimator lowerClamp;
    for (int64_t t = 1'000'000'000; t <= 1'002'000'000; t += 1'000'000)
        expect(lowerClamp.observe(t) == TransportIntervalEstimator::kDefaultNs, "lower clamp estimator warmup");
    expect(lowerClamp.observe(1'003'000'000) == TransportIntervalEstimator::kMinimumNs, "transport clamps below 8 ms");
    TransportIntervalEstimator upperClamp;
    for (int64_t t = 1'000'000'000; t <= 1'100'000'000; t += 50'000'000)
        expect(upperClamp.observe(t) == TransportIntervalEstimator::kDefaultNs, "upper clamp estimator warmup");
    expect(upperClamp.observe(1'150'000'000) == TransportIntervalEstimator::kMaximumNs, "transport clamps above 35 ms");

    const auto corrected = PushPacingRepair::Result{11, true, 1};
    const auto times = repaired_times(1'234, 5'000, corrected);
    expect(times && times->repairedContentPtsNs == 1'334 && times->phaseArrivalNs == 5'100,
        "conversion preserves sub-100 ns residue");
    expect(!repaired_times(INT64_MAX, 1, corrected), "content timestamp overflow is rejected");
    expect(!repaired_times(1, INT64_MAX, corrected), "phase arrival overflow is rejected");
}

void edge_inputs_and_legacy_parity()
{
    constexpr std::array<int64_t, 4> samples{1'000'000, 900'000, 899'999, 899'998};
    constexpr std::array<int64_t, 4> transports{166'667, 166'667, 0, -1};
    constexpr std::array<int64_t, 4> expected{1'000'000, 1'010'000, 1'020'000, 1'030'000};
    PushPacingRepair invalidTransport;
    int64_t prior = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto result = invalidTransport.apply(samples[i], samples[i], transports[i], false);
        expect(result.disposition == PushPacingRepair::Disposition::emit &&
            result.pts == expected[i] && (!i || result.pts > prior),
            "invalid transport retains the non-boundary output frontier");
        prior = result.pts;
    }

    PushPacingRepair edge;
    const auto nearLimit = edge.apply(INT64_MAX - 9'999, INT64_MAX - 9'999, t60, false);
    const auto noProgress = edge.apply(INT64_MAX - 10'000, INT64_MAX - 10'000, t60, false);
    expect(nearLimit.disposition == PushPacingRepair::Disposition::emit &&
        noProgress.disposition == PushPacingRepair::Disposition::noRepresentableProgress,
        "near-maximum reversal returns checked non-emission");
    const auto boundary = edge.apply(1, 1, t60, true);
    expect(boundary.disposition == PushPacingRepair::Disposition::emit && boundary.pts == 1,
        "explicit boundary resets the output frontier");
    PushPacingRepair representable;
    const auto beforeGuard = representable.apply(INT64_MAX - 10'001, INT64_MAX - 10'001, t60, false);
    const auto guard = representable.apply(INT64_MAX - 10'002, INT64_MAX - 10'002, t60, false);
    expect(guard.disposition == PushPacingRepair::Disposition::emit &&
        guard.pts == beforeGuard.pts + 10'000, "near-maximum guard advances exactly 1 ms when representable");

    constexpr uint64_t arrival = 1'000'000'000;
    for (const uint64_t period : {period_30hz, period_40hz, period_60hz}) {
        const auto lead = period == period_40hz ? period / 3 : period == period_60hz ? 5'000'000 : 0;
        for (uint32_t mult = 2; mult <= 4; ++mult) {
            uint64_t frameArrival = arrival, previousOriginal = 0;
            for (unsigned frame = 0; frame < 12; ++frame) {
                const auto plan = plan_cadence(frameArrival, previousOriginal, period, mult);
                const auto expectedOriginal = previousOriginal ? previousOriginal + period : frameArrival + period + lead;
                expect(plan.periodNs == period && plan.originalNs == expectedOriginal,
                    "OFF baseline keeps canonical period and original target");
                for (uint32_t slot = 0; slot <= mult; ++slot) {
                    const auto expectedTarget = slot ? expectedOriginal - period + period * slot / mult : 0;
                    const auto next = expectedOriginal - period + period * (slot + 1) / mult;
                    const auto expectedCutoff = !slot || slot == mult ? 0 :
                        period == period_60hz && mult == 4 ? next - 250'000 :
                        expectedTarget + 3'000'000 < next ? expectedTarget + 3'000'000 : next;
                    expect(plan.target(slot) == expectedTarget &&
                        plan.synthetic_cutoff(slot) == expectedCutoff,
                        "OFF tuple matches independent legacy formulas for every slot");
                }
                previousOriginal = expectedOriginal;
                frameArrival += period;
            }
        }
    }
    constexpr std::array<uint64_t, 5> targets60x4{
        0, 1'009'166'666, 1'013'333'333, 1'017'500'000, 1'021'666'667};
    constexpr std::array<uint64_t, 5> cutoffs60x4{
        0, 1'013'083'333, 1'017'250'000, 1'021'416'667, 0};
    const auto legacy60x4 = plan_cadence(arrival, 0, period_60hz, 4);
    for (uint32_t slot = 0; slot <= 4; ++slot)
        expect(legacy60x4.target(slot) == targets60x4[slot] &&
            legacy60x4.synthetic_cutoff(slot) == cutoffs60x4[slot],
            "OFF 60 Hz 4x tuple matches frozen expected values");
}

void late_guard_and_repeatability()
{
    PushPacingRepair resetRepair;
    const auto beforeReset = resetRepair.apply(1'000'000, 1'000'000, t60, false);
    resetRepair.reset();
    const auto afterReset = resetRepair.apply(-5'000'000, -5'000'000, t60, false);
    expect(beforeReset.pts == 1'000'000 && afterReset.pts == -5'000'000,
        "reset clears the previous time base");

    PushPacingRepair repair;
    int64_t time = 0;
    PushPacingRepair::Result previous{};
    for (unsigned i = 0; i < 30; ++i) {
        previous = repair.apply(time, time, t60, false);
        time += (i % 2 ? 3 : 1) * t60;
    }
    const int64_t early = previous.pts - previous.shift + 1;
    const auto guarded = repair.apply(early, early, t60, false);
    expect(!guarded.repaired, "guard fixture is a pass-through frame");
    expect(guarded.disposition == PushPacingRepair::Disposition::emit &&
        guarded.pts == previous.pts + 10'000 && guarded.shift == guarded.pts - early,
        "non-boundary pass-through receives exact monotonic guard and correct shift");

    PushPacingRepair first, second;
    int64_t t1 = 0, t2 = 0;
    for (unsigned i = 0; i < 50; ++i) {
        const auto a = first.apply(t1, t1, t60, false);
        const auto b = second.apply(t2, t2, t60, false);
        expect(a.pts == b.pts && a.repaired == b.repaired, "identical input is deterministic");
        t1 += (i % 2 ? 3 : 1) * t60;
        t2 += (i % 2 ? 3 : 1) * t60;
    }
}
} // namespace

int main()
{
    try {
        pattern_30_in_60();
        pattern_40_in_120_and_even_cadence();
        variable_stall_and_boundaries();
        transport_delta_contract();
        edge_inputs_and_legacy_parity();
        late_guard_and_repeatability();
        std::printf("PASS vfg_pacing_repair: %u assertions\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL vfg_pacing_repair after %u assertions: %s\n", checks, error.what());
        return 1;
    }
}
