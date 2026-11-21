// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): This work is based on PushFPS, licensed under GPL-3.0-or-later; port modified 2026-11-21.
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace nr::fg::vfg {
enum class AdaptiveBackend : uint8_t { NativeVfg, PushFlow, AnrExperimental };

[[nodiscard]] constexpr bool selectable_multiplier(AdaptiveBackend backend, uint32_t multiplier) noexcept
{
    switch (backend) {
    case AdaptiveBackend::NativeVfg:
    case AdaptiveBackend::PushFlow:
    case AdaptiveBackend::AnrExperimental:
        return multiplier >= 2 && multiplier <= 4;
    }
    return false;
}

[[nodiscard]] inline uint32_t adaptive_multiplier(AdaptiveBackend backend, uint32_t ceiling,
    double contentFps, double targetFps) noexcept
{
    if (ceiling <= 2) return ceiling;
    if (!std::isfinite(contentFps) || !std::isfinite(targetFps) || contentFps <= 0 || targetFps <= 1)
        return ceiling;
    uint32_t best = 0;
    double bestError = 0;
    for (uint32_t multiplier = 2; multiplier <= ceiling; ++multiplier) {
        if (!selectable_multiplier(backend, multiplier)) continue;
        const double error = std::abs(std::log(contentFps * multiplier / targetFps));
        if (!best || error < bestError - 1e-9) {
            best = multiplier;
            bestError = error;
        }
    }
    return best ? best : ceiling;
}

class DisplayFgBudgetGate {
public:
    explicit DisplayFgBudgetGate(int64_t hold100ns = 5'000'000) noexcept : hold_(hold100ns) {}
    void reset() noexcept { valid_ = pendingValid_ = false; }
    [[nodiscard]] bool update(uint32_t decision, int64_t now) noexcept
    {
        if (!valid_) {
            applied_ = decision;
            valid_ = true;
            pendingValid_ = false;
            return true;
        }
        if (decision == applied_) {
            pendingValid_ = false;
            return false;
        }
        if (!pendingValid_ || pending_ != decision) {
            pending_ = decision;
            since_ = now;
            pendingValid_ = true;
            return false;
        }
        if ((now < since_) || (since_ <= INT64_MAX - hold_ && now < since_ + hold_)) return false;
        applied_ = decision;
        pendingValid_ = false;
        return true;
    }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] uint32_t applied() const noexcept { return applied_; }

private:
    int64_t hold_, since_{};
    uint32_t applied_{}, pending_{};
    bool valid_{}, pendingValid_{};
};

class AdaptiveMultiplierGate {
public:
    explicit AdaptiveMultiplierGate(int64_t hold100ns = 5'000'000) noexcept : gate_(hold100ns) {}
    void reset() noexcept { gate_.reset(); }
    [[nodiscard]] uint32_t update(uint32_t candidate, int64_t now, bool* changed = nullptr) noexcept
    {
        const bool didChange = gate_.update(candidate, now);
        if (changed) *changed = didChange;
        return gate_.applied();
    }
    [[nodiscard]] bool valid() const noexcept { return gate_.valid(); }
    [[nodiscard]] uint32_t applied() const noexcept { return gate_.applied(); }

private:
    DisplayFgBudgetGate gate_;
};

class ContentRateTracker {
public:
    static constexpr std::size_t capacity = 128;
    static constexpr uint64_t windowNs = 750'000'000;

    void reset() noexcept { *this = {}; }
    // staticHold is latched from the upstream content detector's still-hold
    // decision, then cleared on its next "new" decision. Ordinal repeats alone
    // cannot distinguish a long still from ordinary cadence repeats.
    [[nodiscard]] double observe(uint64_t timestampNs, uint64_t ordinal, bool staticHold) noexcept
    {
        if (haveNow_ && timestampNs < lastNowNs_) reset();
        lastNowNs_ = timestampNs;
        haveNow_ = true;
        while (count_ && timestampNs - samples_[head_].timestampNs > windowNs) {
            head_ = (head_ + 1) % capacity;
            --count_;
        }
        if (staticHold) {
            if (!staticHoldActive_) clear_samples();
            staticHoldActive_ = true;
            return 0;
        }
        if (staticHoldActive_) {
            clear_samples();
            staticHoldActive_ = false;
        }
        if (haveOrdinal_ && ordinal == lastOrdinal_) return fps();
        if ((haveOrdinal_ && ordinal < lastOrdinal_) ||
            (count_ && (timestampNs <= samples_[(head_ + count_ - 1) % capacity].timestampNs ||
                ordinal <= samples_[(head_ + count_ - 1) % capacity].ordinal))) {
            head_ = count_ = 0;
            haveOrdinal_ = false;
        }
        lastOrdinal_ = ordinal;
        haveOrdinal_ = true;
        if (count_ == capacity) {
            head_ = (head_ + 1) % capacity;
            --count_;
        }
        samples_[(head_ + count_) % capacity] = {timestampNs, ordinal};
        ++count_;
        return fps();
    }
    [[nodiscard]] double fps() const noexcept
    {
        if (count_ < 8) return 0;
        const auto& first = samples_[head_];
        const auto& last = samples_[(head_ + count_ - 1) % capacity];
        const auto spanNs = last.timestampNs - first.timestampNs;
        const auto ordinalDelta = last.ordinal - first.ordinal;
        return spanNs && ordinalDelta ? double(ordinalDelta) * 1'000'000'000.0 / double(spanNs) : 0;
    }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    struct Sample { uint64_t timestampNs{}, ordinal{}; };
    void clear_samples() noexcept { head_ = count_ = 0; }

    std::array<Sample, capacity> samples_{};
    std::size_t head_{}, count_{};
    uint64_t lastNowNs_{}, lastOrdinal_{};
    bool haveNow_{}, haveOrdinal_{}, staticHoldActive_{};
};

[[nodiscard]] inline double query_monitor_refresh_hz(const wchar_t* deviceName) noexcept
{
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1)
        return 0;
    return double(mode.dmDisplayFrequency);
}
} // namespace nr::fg::vfg
