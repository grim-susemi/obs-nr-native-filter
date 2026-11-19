// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_vfg_session.h"
#include <string>

namespace nr::fg::vfg {
struct LaunchOptions {
    uint32_t multiplier = 2; // 1 = off (original), 2..4 = VFG
    uint32_t quality = 1;    // 0 = Best, 1 = Balanced, 2 = Fast
    std::wstring monitorId;  // empty = auto
    uint32_t fpsOverlay = 1;  // 0 = off, 1 = top-left, 2 = top-right
    uint32_t lowLatency = 1;  // 0 = off, 1 = NVIDIA Reflex on
    uint32_t gpuPriority = 1; // 0 = normal, 1 = high
};
class Controller final {
    HANDLE job_{}, child_{}, mapping_{};
    Session* view_{};
    LaunchOptions options_;
public:
    explicit Controller(const LaunchOptions& options);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    ipc::Peer peer() const { return ipc::identity(child_); }
    bool alive() const { return WaitForSingleObject(child_, 0) == WAIT_TIMEOUT; }
    bool exited() const { return WaitForSingleObject(child_, 0) == WAIT_OBJECT_0; }
    bool offer(const ipc::Offer&, const ipc::Shape&, LUID) noexcept;
    void stop() noexcept { if (view_) InterlockedExchange64(&view_->state, -1); }
    void set_fps_overlay(uint32_t pos) noexcept { if (view_) InterlockedExchange(&view_->fpsOverlay, static_cast<LONG>(pos)); }
    void set_low_latency(uint32_t on) noexcept { if (view_) InterlockedExchange(&view_->lowLatency, static_cast<LONG>(on)); }
    static std::wstring find_runtime_dir();
};
} // namespace nr::fg::vfg
