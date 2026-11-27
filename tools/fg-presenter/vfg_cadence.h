// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace nr::fg::vfg {
inline constexpr uint64_t period_120hz = 8'333'333, period_60hz = 16'666'667, period_50hz = 20'000'000,
    period_40hz = 25'000'000, period_30hz = 33'333'333, period_24hz = 41'666'667;
inline constexpr int64_t clock_stall_ns = 500'000'000;
namespace detail {
struct CadenceBand { uint64_t periodNs, toleranceNs; };
inline constexpr std::array<CadenceBand, 6> cadence_bands{{
    {period_120hz, 1'200'000}, {period_60hz, 2'750'000}, {period_50hz, 300'000},
    {period_40hz, 2'750'000}, {period_30hz, 2'750'000}, {period_24hz, 2'000'000}}};
} // namespace detail

class UniqueCadence {
    std::array<uint64_t, 8> intervals_{};
    std::size_t count_{}, next_{};
    std::optional<uint64_t> last_;
    uint64_t total_{};
public:
    void reset() noexcept { *this = {}; }

    // Observe only unique originals on one monotonic clock (prefer the OBS
    // output tick if it is supplied consistently). Repeated OBS ticks of the
    // same image are not samples. A 40 Hz source on 60 Hz transport gives
    // alternating ~17/33 ms intervals, so use the mean rather than the median.
    [[nodiscard]] uint64_t observe(uint64_t timestampNs) noexcept
    {
        if (!last_ || timestampNs <= *last_ || timestampNs - *last_ > 50'000'000 ||
            timestampNs - *last_ < 6'000'000) {
            reset();
            last_ = timestampNs;
            return 0;
        }
        if (count_ == intervals_.size()) total_ -= intervals_[next_];
        else ++count_;
        intervals_[next_] = timestampNs - *last_;
        total_ += intervals_[next_];
        next_ = (next_ + 1) % intervals_.size();
        last_ = timestampNs;
        if (count_ < 4) return 0;
        const auto mean = total_ / count_;
        for (const auto [period, tolerance] : detail::cadence_bands) {
            const auto error = mean > period ? mean - period : period - mean;
            if (error <= tolerance) return period;
        }
        return 0;
    }
};

struct CadencePlan {
    uint64_t originalNs{}, periodNs{};
    uint32_t multiplier{};
    // Slots 1..multiplier-1 are synthetic; multiplier is the original.
    [[nodiscard]] uint64_t target(uint32_t slot) const noexcept
    {
        return slot && slot <= multiplier ? originalNs - periodNs + periodNs * slot / multiplier : 0;
    }

    [[nodiscard]] uint64_t synthetic_cutoff(uint32_t slot) const noexcept
    {
        if (!slot || slot >= multiplier) return 0;
        const auto due = target(slot);
        const auto next = target(slot + 1);
        if (periodNs == period_120hz) return next - 150'000;
        if (periodNs == period_60hz && multiplier == 4)
            return next - 250'000;
        return due + 3'000'000 < next ? due + 3'000'000 : next;
    }
};

// A confirmed unique pair one native tick apart does not need to wait four
// intervals. A repeat disarms this fallback until the estimator locks again.
[[nodiscard]] inline bool provisional_120hz(uint64_t a, uint64_t b, bool sawRepeat) noexcept
{
    return !sawRepeat && b > a && b - a >= 7'000'000 && b - a <= 9'600'000;
}

[[nodiscard]] inline bool provisional_60hz(uint64_t a, uint64_t b, bool sawRepeat) noexcept
{
    return !sawRepeat && b > a && b - a >= 14'000'000 && b - a <= 19'000'000;
}

[[nodiscard]] inline CadencePlan plan_cadence(uint64_t arrivalNs, uint64_t previousOriginalTargetNs,
    uint64_t periodNs, uint32_t multiplier) noexcept
{
    if (std::none_of(detail::cadence_bands.begin(), detail::cadence_bands.end(),
            [periodNs](detail::CadenceBand band) { return periodNs == band.periodNs; }) ||
        multiplier < 2 || multiplier > 4 || arrivalNs > UINT64_MAX - 2 * periodNs)
        return {};
    // Preserve phase across the 40 Hz source's non-uniform transport ticks.
    // On startup, its one-tick quantization needs up to 1/3 period extra lead.
    // On the first 60 Hz pair the SDK starts with cold interpolation state.
    // Keep the ensuing slots even instead of dropping the first subframe
    // when the initial enqueue outlasts its nominal 4x slot.
    const auto lead = periodNs == period_40hz ? periodNs / 3 : periodNs == period_60hz ? 5'000'000 : 0;
    const auto seed = arrivalNs + periodNs + lead;
    if (previousOriginalTargetNs && previousOriginalTargetNs <= UINT64_MAX - periodNs) {
        const auto proposed = previousOriginalTargetNs + periodNs;
        if (proposed >= arrivalNs + 3 * periodNs / 4 && proposed <= arrivalNs + 7 * periodNs / 4)
            return {proposed, periodNs, multiplier};
    }
    return {seed, periodNs, multiplier};
}

[[nodiscard]] inline CadencePlan plan_cadence_adaptive(uint64_t arrivalNs,
    uint64_t previousOriginalTargetNs, uint64_t contentPeriodNs, uint32_t effectiveMult) noexcept
{
    if (contentPeriodNs < 7'500'000 || contentPeriodNs > 50'000'000 ||
        effectiveMult < 2 || effectiveMult > 4 || arrivalNs > UINT64_MAX - 2 * contentPeriodNs)
        return {};
    if (previousOriginalTargetNs && previousOriginalTargetNs <= UINT64_MAX - contentPeriodNs) {
        const auto proposed = previousOriginalTargetNs + contentPeriodNs;
        if (proposed >= contentPeriodNs && proposed >= arrivalNs + 3 * contentPeriodNs / 4 &&
            proposed <= arrivalNs + 7 * contentPeriodNs / 4)
            return {proposed, contentPeriodNs, effectiveMult};
    }
    return {arrivalNs + contentPeriodNs, contentPeriodNs, effectiveMult};
}
} // namespace nr::fg::vfg
