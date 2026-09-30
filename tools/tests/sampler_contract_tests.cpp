// SPDX-License-Identifier: GPL-3.0-only
#include "obs_filter.h"
#include <cstdio>
#include <stdexcept>
static void require(bool ok) { if (!ok) throw std::runtime_error("sampler contract failed"); }
int main()
{
    using namespace nr::sampler;
    require(admit(true, true, 64, 64, true) == Admission::Ready);
    require(admit(false, true, 64, 64, true) == Admission::NoTarget);
    require(admit(true, true, 0, 64, true) == Admission::ZeroExtent);
    require(admit(true, false, 64, 64, true) == Admission::UnsupportedRenderer);
    require(admit(true, true, 64, 64, false) == Admission::UnsupportedColor);
    require(canonical_tick(1000000000, 16666666) == 61);
    require(canonical_tick(1000000000, 0) == 0);
    nr::PerInstanceTick tick(7);
    for (uint64_t t = 61; t < 65; ++t) {
        require(tick.claim(t) == nr::TickAction::CaptureOnce);
        const nr::FrameKey key{7, 1, t - 60};
        const nr::Selection selected{t, nr::OutputKind::PrimingRaw, nr::Status::Bypassed,
                                    nr::Reason::None, key, std::nullopt, false};
        require(tick.latch(t, selected));
        for (int view = 0; view < 5; ++view) {
            require(tick.claim(t) == nr::TickAction::ReuseLatch);
            require(tick.peek(t)->frame == key);
        }
    }
    require(tick.claim(63) == nr::TickAction::Reject);
    std::puts("PASS sampler admission and canonical all-view latch");
}
