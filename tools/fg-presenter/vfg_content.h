// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg/fg_ipc.h"

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
    [[nodiscard]] unsigned input_index() const noexcept
    {
        unsigned index = 0;
        while ((transport_ && transport_->index == index) || (anchor_ && anchor_->index == index))
            ++index;
        return index; // At most two of the three inputs are reserved.
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

    // Call only after a successful original Present, using the admitted input.
    void retain(unsigned index, const ipc::Frame& frame) noexcept
    {
        anchor_ = ContentFrame{frame, index};
    }

    // Keep the IPC destination reservation even while there is no pair anchor.
    void invalidate() noexcept { anchor_.reset(); }
};
} // namespace nr::fg::vfg
