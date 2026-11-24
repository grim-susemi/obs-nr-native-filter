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
    struct ChildSession;
    std::unique_ptr<ChildSession> session_;
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
    ipc::Peer peer() const;
    bool alive() const noexcept;
    bool exited() const noexcept;
    bool await_exit_cleanup(DWORD timeoutMs = 300) const noexcept;
    bool user_close_marker_cleaned() const noexcept;
    bool offer(const ipc::Offer&, const ipc::Shape&, LUID) noexcept;
    bool request_resize() noexcept;
    bool resize_drained() const noexcept;
    void request_stop(bool userStop = false) noexcept;
    void stop(bool userStop = false) noexcept;
    ExitReason exit_reason() const noexcept;
    bool is_fault_or_watchdog_exit() const noexcept;
    bool can_recover() const noexcept;
    bool begin_async_fallback_relaunch(bool forceFaultOnTimeout = false) noexcept;
    RelaunchState poll_async_fallback_relaunch() noexcept;
    void set_fps_overlay(uint32_t pos) noexcept;
    void set_low_latency(uint32_t on) noexcept;
    void set_hide_from_capture(uint32_t hide) noexcept;
    void set_hud_lock(uint32_t on) noexcept;
    void set_pacing_repair(uint32_t on) noexcept;
    void set_adaptive_fg(uint32_t on) noexcept;
    void set_adaptive_target_fps(uint32_t fps) noexcept;
    static std::wstring find_runtime_dir();
    static std::wstring find_pushflow_dir(PushFlowTier tier);
};
} // namespace nr::fg::vfg
