// SPDX-License-Identifier: GPL-3.0-only
// Adapted from PushFPS PushPacingRepair.h; port modified 2026-11-21. Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// Original source: https://github.com/DosukaSOL/PushFPS/blob/4e5c0491d0f5f7c01f1c4718e2b156857573abcd/include/veyra/engine/PushPacingRepair.h
#pragma once
#include "vfg_cadence.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <optional>

namespace nr::fg::vfg {
class PushPacingRepair {
public:
    static constexpr unsigned kWindow = 16;
    static constexpr unsigned kMinimum = 6;
    enum class Disposition { emit, noRepresentableProgress };
    struct Result {
        int64_t pts = 0;
        bool repaired = false;
        int64_t shift = 0;
        // On noRepresentableProgress, pts is invalid for submission: drop this
        // frame and wait for a later input or an explicit boundary before emitting.
        Disposition disposition = Disposition::emit;
    };
    struct Stats {
        uint64_t frames = 0, repaired = 0, restarts = 0, variable = 0;
        int64_t maxLate = 0, maxEarly = 0;
    };

    void reset() { restart(); haveLast_ = false; }
    [[nodiscard]] const Stats& stats() const { return stats_; }

    [[nodiscard]] Result apply(int64_t sample, int64_t content, int64_t transport, bool boundary)
    {
        ++stats_.frames;
        Result result{content, false, safe_difference(content, sample)};
        if (boundary) {
            reset();
            push(sample);
            return keep(result);
        }
        if (transport <= 0) {
            restart();
            push(sample);
            return keep(result);
        }
        if (haveLast_ && sample <= lastSample_) {
            restart();
            push(sample);
            return keep(result);
        }
        push(sample);
        if (count_ < kMinimum) return keep(result);

        double slope = 0, intercept = 0;
        fit(slope, intercept);
        const double tolerance = static_cast<double>(transport) * 1.1;
        const double newest = intercept + slope * static_cast<double>(count_ - 1);
        if (std::abs(static_cast<double>(sample) - newest) > tolerance) {
            ++stats_.restarts;
            restart();
            push(sample);
            return keep(result);
        }
        if (slope < static_cast<double>(transport) * 1.4) return keep(result);
        for (unsigned i = 0; i < count_; ++i) {
            const double line = intercept + slope * static_cast<double>(i);
            if (std::abs(static_cast<double>(at(i)) - line) > tolerance) {
                ++stats_.variable;
                return keep(result);
            }
        }

        double target = newest;
        if (lastRepaired_)
            target = static_cast<double>(lastOut_) + slope +
                0.2 * (newest - (static_cast<double>(lastOut_) + slope));
        const auto rounded = target + (target >= 0 ? 0.5 : -0.5);
        if (rounded < static_cast<double>(INT64_MIN) || rounded >= 9'223'372'036'854'774'784.0) {
            ++stats_.restarts;
            restart();
            push(sample);
            return keep(result);
        }
        int64_t pts = static_cast<int64_t>(rounded);
        if (sample < INT64_MIN + transport || sample > INT64_MAX - transport) {
            ++stats_.restarts;
            restart();
            push(sample);
            return keep(result);
        }
        pts = std::clamp<int64_t>(pts, sample - transport, sample + transport);
        if (haveLast_ && (slope / 2 > static_cast<double>(INT64_MAX) ||
            lastOut_ > INT64_MAX - static_cast<int64_t>(slope / 2))) {
            ++stats_.restarts;
            restart();
            push(sample);
            return keep(result);
        }
        if (haveLast_) pts = std::max<int64_t>(pts, lastOut_ + static_cast<int64_t>(slope / 2));
        result.pts = pts;
        result.repaired = true;
        result.shift = safe_difference(pts, sample);
        lastRepaired_ = true;
        const auto output = keep(result);
        if (output.disposition == Disposition::emit) {
            ++stats_.repaired;
            stats_.maxLate = std::max(stats_.maxLate, output.shift);
            stats_.maxEarly = std::max(stats_.maxEarly, -output.shift);
        }
        return output;
    }

private:
    [[nodiscard]] static int64_t safe_difference(int64_t left, int64_t right)
    {
        if (right < 0 && left > INT64_MAX + right) return INT64_MAX;
        if (right > 0 && left < INT64_MIN + right) return INT64_MIN;
        return left - right;
    }
    void restart() { count_ = head_ = 0; lastRepaired_ = false; }
    [[nodiscard]] Result keep(const Result& result)
    {
        if (!result.repaired) lastRepaired_ = false;
        Result output = result;
        if (haveLast_ && output.pts <= lastOut_) {
            if (lastOut_ > INT64_MAX - 10000) {
                output.disposition = Disposition::noRepresentableProgress;
                lastRepaired_ = false;
                lastSample_ = count_ ? at(count_ - 1) : lastSample_;
                haveLast_ = true;
                return output;
            }
            const int64_t sample = safe_difference(result.pts, result.shift);
            output.pts = lastOut_ + 10000;
            output.shift = safe_difference(output.pts, sample);
        }
        lastOut_ = output.pts;
        lastSample_ = count_ ? at(count_ - 1) : lastSample_;
        haveLast_ = true;
        return output;
    }
    void push(int64_t time)
    {
        times_[head_] = time;
        head_ = (head_ + 1) % kWindow;
        count_ = std::min(count_ + 1, kWindow);
    }
    [[nodiscard]] int64_t at(unsigned index) const
    {
        return times_[(head_ + kWindow - count_ + index) % kWindow];
    }
    void fit(double& slope, double& intercept) const
    {
        const double origin = static_cast<double>(at(0));
        const double n = static_cast<double>(count_);
        const double meanX = (n - 1) / 2;
        double meanY = 0;
        for (unsigned i = 0; i < count_; ++i) meanY += static_cast<double>(at(i)) - origin;
        meanY /= n;
        double sxx = 0, sxy = 0;
        for (unsigned i = 0; i < count_; ++i) {
            const double x = static_cast<double>(i) - meanX;
            const double y = static_cast<double>(at(i)) - origin - meanY;
            sxx += x * x;
            sxy += x * y;
        }
        slope = sxx > 0 ? sxy / sxx : 0;
        intercept = origin + meanY - slope * meanX;
    }
    std::array<int64_t, kWindow> times_{};
    unsigned count_ = 0, head_ = 0;
    int64_t lastOut_ = 0, lastSample_ = 0;
    bool haveLast_ = false, lastRepaired_ = false;
    Stats stats_{};
};

struct RepairedTimes {
    int64_t repairedContentPtsNs = 0;
    int64_t phaseArrivalNs = 0;
};

[[nodiscard]] inline std::optional<RepairedTimes> repaired_times(
    int64_t sampleNs, int64_t arrivalNs, const PushPacingRepair::Result& result)
{
    if (result.shift > INT64_MAX / 100 || result.shift < INT64_MIN / 100) return std::nullopt;
    const int64_t shiftNs = result.shift * 100;
    if ((shiftNs > 0 && sampleNs > INT64_MAX - shiftNs) ||
        (shiftNs < 0 && sampleNs < INT64_MIN - shiftNs) ||
        (shiftNs > 0 && arrivalNs > INT64_MAX - shiftNs) ||
        (shiftNs < 0 && arrivalNs < INT64_MIN - shiftNs)) return std::nullopt;
    return RepairedTimes{sampleNs + shiftNs, arrivalNs + shiftNs};
}

class TransportIntervalEstimator {
public:
    static constexpr unsigned kWindow = 9;
    static constexpr int64_t kMinimumNs = 4'000'000;
    static constexpr int64_t kMaximumNs = 50'000'000;
    static constexpr int64_t kDefaultNs = 16'666'667;

    [[nodiscard]] int64_t observe(int64_t sample)
    {
        if (haveLast_ && (sample <= last_ ||
            (last_ <= INT64_MAX - clock_stall_ns && sample > last_ + clock_stall_ns))) reset();
        if (haveLast_) {
            deltas_[next_] = (std::clamp)(sample - last_, kMinimumNs, kMaximumNs);
            next_ = (next_ + 1) % kWindow;
            count_ = (std::min)(count_ + 1, kWindow);
        }
        last_ = sample;
        haveLast_ = true;
        if (count_ < 3) return kDefaultNs;
        auto sorted = deltas_;
        const auto size = count_;
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(size));
        if (size % 2) return sorted[size / 2];
        return sorted[size / 2 - 1] + (sorted[size / 2] - sorted[size / 2 - 1]) / 2;
    }
    void reset() { deltas_ = {}; count_ = next_ = 0; last_ = 0; haveLast_ = false; }

private:
    std::array<int64_t, kWindow> deltas_{};
    unsigned count_ = 0, next_ = 0;
    int64_t last_ = 0;
    bool haveLast_ = false;
};
} // namespace nr::fg::vfg
