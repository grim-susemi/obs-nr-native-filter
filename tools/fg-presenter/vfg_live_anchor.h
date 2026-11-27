// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "vfg_cadence.h"
#include <cmath>

namespace nr::fg::vfg {
class LiveAnchorClock {
public:
    struct Sample { uint64_t contentPtsNs, callbackNs; }; // QPC converted to nanoseconds by the caller
    struct Timing {
        uint64_t anchorNs{}, realTargetNs{}, snaps{};
        double clockDriftPpm{}, callbackMinusAnchorMs{}, callbackJitterMs{}, phaseMs{}, deadlineExtraMs{};
        bool locked{}, fgHeld{};
        [[nodiscard]] uint64_t deadline_extra_ns() const noexcept
        {
            return static_cast<uint64_t>(std::llround(deadlineExtraMs * 1e6));
        }
    };

    void reset() noexcept { *this = {}; }

    // Observe each unique source/dedup PTS once, in capture order. phaseNs is the
    // desired real-frame lead on the host clock; deadline slack is separate.
    [[nodiscard]] Timing observe(Sample now, uint64_t phaseNs) noexcept
    {
        if (count_ && discontinuous(now)) {
            const auto snaps = timing_.snaps + 1;
            reset();
            timing_.snaps = snaps;
        }
        last_ = now;
        window_[next_] = now;
        next_ = (next_ + 1) % window_.size();
        count_ = (std::min)(count_ + 1, window_.size());

        // Regress host-minus-content elapsed time on content elapsed time.
        // Subtract integer epochs first: absolute QPC or PTS may exceed 2^53.
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const double x = static_cast<double>(now.contentPtsNs - window_[i].contentPtsNs);
            const double y = static_cast<double>(now.callbackNs - window_[i].callbackNs) - x;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const auto n = static_cast<double>(count_);
        const double xMean = sx / n, yMean = sy / n;
        const auto& oldest = window_[count_ == window_.size() ? next_ : 0];
        // A few adjacent callbacks can turn sub-ms jitter into thousands of
        // spurious ppm. Acquire frequency only across a quarter-second aperture.
        timing_.locked = count_ >= 4 && now.contentPtsNs - oldest.contentPtsNs >= 250'000'000;
        const double skew = timing_.locked ?
            (std::clamp)((sxy - sx * yMean) / (sxx - sx * xMean), -0.005, 0.005) : 0;
        timing_.clockDriftPpm = skew * 1e6; // positive: more host time elapsed per unit of PTS
        const auto lag = std::llround(yMean - skew * xMean);
        const auto fitted = lag >= 0 ? now.callbackNs - (std::min)(now.callbackNs, static_cast<uint64_t>(lag)) :
            add_ns(now.callbackNs, static_cast<uint64_t>(-lag));
        timing_.anchorNs = count_ == 1 ? fitted : (std::max)(fitted, add_ns(timing_.anchorNs, 1));
        timing_.callbackMinusAnchorMs = now.callbackNs >= timing_.anchorNs ?
            static_cast<double>(now.callbackNs - timing_.anchorNs) / 1e6 :
            -static_cast<double>(timing_.anchorNs - now.callbackNs) / 1e6;
        timing_.callbackJitterMs += (std::abs(timing_.callbackMinusAnchorMs) - timing_.callbackJitterMs) / 8;
        timing_.deadlineExtraMs = (std::min)(timing_.callbackJitterMs, 1.0);
        timing_.phaseMs = static_cast<double>(phaseNs) / 1e6;
        phasedTargetNs_ = (std::max)(now.callbackNs, add_ns(timing_.anchorNs, phaseNs));
        return real_target();
    }

    // Resolve this original's FG admission before observing the next original.
    // A held frame must still probe admission: the first success restores phase.
    [[nodiscard]] Timing resolve_fg(bool accepted) noexcept
    {
        if (accepted) rejectingSince_.reset();
        else if (count_ && !rejectingSince_) rejectingSince_ = last_.callbackNs;
        return real_target();
    }

private:
    [[nodiscard]] Timing real_target() noexcept
    {
        timing_.fgHeld = rejectingSince_ && last_.callbackNs - *rejectingSince_ >= 1'000'000'000;
        timing_.realTargetNs = timing_.fgHeld ? last_.callbackNs : phasedTargetNs_;
        return timing_;
    }
    [[nodiscard]] static uint64_t add_ns(uint64_t time, uint64_t duration) noexcept
    {
        return time + (std::min)(duration, UINT64_MAX - time);
    }
    [[nodiscard]] bool discontinuous(Sample now) const noexcept
    {
        if (now.contentPtsNs <= last_.contentPtsNs || now.callbackNs <= last_.callbackNs) return true;
        const auto pts = now.contentPtsNs - last_.contentPtsNs, host = now.callbackNs - last_.callbackNs;
        if (pts > clock_stall_ns || host > clock_stall_ns) return true;
        // A phase step larger than an entire supported period is a new time
        // base, not oscillator drift. Do not contaminate the fit or hold timer.
        const double expected = static_cast<double>(pts) * (1 + timing_.clockDriftPpm / 1e6);
        return std::abs(static_cast<double>(host) - expected + timing_.callbackMinusAnchorMs * 1e6) > 50'000'000;
    }

    std::array<Sample, 64> window_{};
    std::size_t next_{}, count_{};
    Sample last_{};
    std::optional<uint64_t> rejectingSince_;
    Timing timing_{};
    uint64_t phasedTargetNs_{};
};
} // namespace nr::fg::vfg
