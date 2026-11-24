// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_vfg_session.h"
#include <string>
#include <atomic>
#include <memory>
#include <thread>

namespace nr::fg::vfg {
struct LaunchOptions {
    uint32_t multiplier = 2; // 1 = off (original), 2..4 = VFG
    Quality quality = Quality::Medium;
    std::wstring monitorId;  // empty = auto
    uint32_t fpsOverlay = 1;  // 0 = off, 1 = top-left, 2 = top-right
    uint32_t lowLatency = 1;  // 0 = off, 1 = NVIDIA Reflex on
    uint32_t gpuPriority = 1; // 0 = normal, 1 = high
    uint32_t hideFromCapture = 0; // 0 = capturable (WDA_NONE), 1 = hide from capture (0x11)
    Backend backend{};
    PushFlowTier pushflowTier{};
    uint32_t gpuShotChange{}, experimentalAnrOptIn{};
    std::wstring pushflowRuntimeDir, anrDllPath;
    uint32_t hudLock{}, pacingRepair{}, adaptiveFg{}, adaptiveTargetFps{};
};
enum class RelaunchState { Idle, InFlight, Ready, Failed };
class Controller final {
    HANDLE job_{}, child_{}, mapping_{};
    Session* view_{};
    LaunchOptions options_;
    std::atomic<RelaunchState> relaunchState_{RelaunchState::Idle};
    std::atomic_bool stopRequested_{false}, wakeRelaunch_{false};
    bool fallbackRestarted_{};
    uint64_t launchEpoch_{1};
    std::unique_ptr<Controller> replacement_;
    std::jthread relaunchWorker_;
    Controller(const LaunchOptions&, bool fallback);
    void run_fallback(LaunchOptions options);
    void sync_live_settings() noexcept;
public:
    explicit Controller(const LaunchOptions& options);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    ipc::Peer peer() const { return ipc::identity(child_); }
    bool alive() const { return WaitForSingleObject(child_, 0) == WAIT_TIMEOUT; }
    bool exited() const { return relaunchState_.load() != RelaunchState::InFlight &&
        relaunchState_.load() != RelaunchState::Ready && WaitForSingleObject(child_, 0) == WAIT_OBJECT_0; }
    bool offer(const ipc::Offer&, const ipc::Shape&, LUID) noexcept;
    bool request_resize() noexcept { return InterlockedCompareExchange64(&view_->state, 2, 1) == 1; }
    bool resize_drained() const noexcept { return InterlockedCompareExchange64(&view_->state, 3, 3) == 3; }
    void request_stop(bool userStop = false) noexcept;
    void stop(bool userStop = false) noexcept;
    ExitReason exit_reason() const noexcept { return read_exit_reason(*view_); }
    bool is_fault_or_watchdog_exit() const noexcept;
    bool can_recover() const noexcept;
    bool begin_async_fallback_relaunch(bool forceFaultOnTimeout = false) noexcept;
    RelaunchState poll_async_fallback_relaunch() noexcept;
    void set_fps_overlay(uint32_t pos) noexcept {
        options_.fpsOverlay = pos;
        if (view_) InterlockedExchange(&view_->fpsOverlay, static_cast<LONG>(pos));
    }
    void set_low_latency(uint32_t on) noexcept {
        options_.lowLatency = on;
        if (view_) InterlockedExchange(&view_->lowLatency, static_cast<LONG>(on));
    }
    void set_hide_from_capture(uint32_t hide) noexcept {
        options_.hideFromCapture = hide ? 1u : 0u;
        if (view_) InterlockedExchange(&view_->hideFromCapture, static_cast<LONG>(options_.hideFromCapture));
    }
    void set_hud_lock(uint32_t on) noexcept {
        options_.hudLock = on != 0;
        InterlockedExchange(&view_->hudLock, static_cast<LONG>(options_.hudLock));
    }
    void set_pacing_repair(uint32_t on) noexcept {
        options_.pacingRepair = on != 0;
        InterlockedExchange(&view_->pacingRepair, static_cast<LONG>(options_.pacingRepair));
    }
    void set_adaptive_fg(uint32_t on) noexcept {
        options_.adaptiveFg = on != 0;
        InterlockedExchange(&view_->adaptiveFg, static_cast<LONG>(options_.adaptiveFg));
    }
    void set_adaptive_target_fps(uint32_t fps) noexcept {
        options_.adaptiveTargetFps = fps <= LONG_MAX ? fps : 0;
        InterlockedExchange(&view_->adaptiveTargetFps, static_cast<LONG>(options_.adaptiveTargetFps));
    }
    static std::wstring find_runtime_dir();
    static std::wstring find_pushflow_dir(PushFlowTier tier);
};
} // namespace nr::fg::vfg
