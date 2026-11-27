// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg/fg_ipc.h"
#include <span>

namespace nr::fg::vfg {
inline constexpr unsigned content_input_count = 3;

struct ContentFrame {
    ipc::Frame frame;
    unsigned index;
};

struct ContentStep {
    bool stale;
    bool continuous;
};

// Veyra parity: up to five over-band RGB components per million count as
// unchanged. The floor keeps one component of slack on frames whose fractional
// budget rounds below one. Over-band components are produced by
// vfg_content_compare_band.hlsl, which measures 8-bit LSB steps.
[[nodiscard]] inline unsigned content_over_budget(unsigned width, unsigned height) noexcept
{
    const uint64_t over = static_cast<uint64_t>(width) * height * 3ULL * 5ULL / 1'000'000ULL;
    return static_cast<unsigned>(over > 1ULL ? over : 1ULL);
}

// Repeat classification controls synthesis and HUD learning, not 1x playback.
template<typename Present>
void present_content_repeat(bool interpolate, bool split, const Present& present)
{
    if (!interpolate || split) present();
}

// The transport predecessor and the last presented unique original are distinct
// owners. Repeated content advances the former without replacing the latter.
class ContentHistory {
    std::optional<ContentFrame> transport_, anchor_;

    static bool adjacent(const ipc::Frame& a, const ipc::Frame& b) noexcept
    {
        const auto& x = a.shape;
        const auto& y = b.shape;
        return a.generation == b.generation && b.serial > a.serial && b.serial - a.serial == 1 &&
            b.timestampNs > a.timestampNs && x.width == y.width && x.height == y.height &&
            x.format == y.format && x.colorSpace == y.colorSpace &&
            x.sourceMonitor == y.sourceMonitor && x.displayCapture == y.displayCapture;
    }
public:
    // A slot is free when it is neither a live IPC destination nor a history
    // entry the caller pinned. content_input_count means every slot is reserved:
    // wait for a pending frame to be presented before submitting another copy.
    // With empty pins at most two slots are reserved, so the zero-argument
    // callers always receive a real slot.
    [[nodiscard]] unsigned input_index(std::span<const unsigned> pinnedIndices = {}) const noexcept
    {
        for (unsigned index = 0; index < content_input_count; ++index) {
            if (transport_ && transport_->index == index) continue;
            if (anchor_ && anchor_->index == index) continue;
            bool pinned = false;
            for (const unsigned reserved : pinnedIndices)
                if (reserved == index) pinned = true;
            if (!pinned) return index;
        }
        return content_input_count;
    }

    [[nodiscard]] ContentStep observe(unsigned index, const ipc::Frame& frame, bool transportAdjacent) noexcept
    {
        if (transport_ && transport_->frame.generation == frame.generation &&
            frame.serial <= transport_->frame.serial)
            return {true, false};
        const bool continuous = transportAdjacent && transport_ && adjacent(transport_->frame, frame);
        if (!continuous) invalidate();
        transport_ = ContentFrame{frame, index};
        return {false, continuous};
    }

    [[nodiscard]] bool can_compare() const noexcept { return anchor_.has_value(); }
    [[nodiscard]] const std::optional<ContentFrame>& anchor() const noexcept { return anchor_; }
    // Newest input admitted by observe, including one still waiting for present.
    // A pending-pair queue chains from this frame before retain() advances anchor().
    [[nodiscard]] const std::optional<ContentFrame>& latest_admitted() const noexcept { return transport_; }

    // Call only after a successful original Present, using the admitted input.
    void retain(unsigned index, const ipc::Frame& frame) noexcept
    {
        anchor_ = ContentFrame{frame, index};
    }

    // Keep the IPC destination reservation even while there is no pair anchor.
    void invalidate() noexcept { anchor_.reset(); }
};
} // namespace nr::fg::vfg
