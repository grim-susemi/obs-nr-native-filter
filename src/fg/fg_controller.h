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
    uint32_t dedupMode = 0; // DedupMode: 0 = off/exact, 1 = auto, 5 = 60->30 adaptive, 6 = 60->30 fixed
    uint32_t liveAnchor = 0; // 0 = off, 1 = live clock phase-lock
    uint64_t transportIntervalNs = 0; // OBS video tick in ns; 0 = presenter default pacing
};
struct LiveSettings {
    uint32_t dedupMode{};
    uint32_t liveAnchor{};
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
    HANDLE relaunchSettledEvent_{};
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
    bool log_write_in_flight() const noexcept;
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
    // Blocks until an in-flight fallback relaunch settles (Ready/Failed) or the
    // timeout expires. poll_async_fallback_relaunch() still consumes Ready.
    bool await_async_fallback_relaunch(DWORD timeoutMs) noexcept;
    void set_fps_overlay(uint32_t pos) noexcept;
    void set_low_latency(uint32_t on) noexcept;
    void set_hide_from_capture(uint32_t hide) noexcept;
    void set_hud_lock(uint32_t on) noexcept;
    void set_pacing_repair(uint32_t on) noexcept;
    void set_adaptive_fg(uint32_t on) noexcept;
    void set_adaptive_target_fps(uint32_t fps) noexcept;
    void set_dedup_mode(uint32_t mode) noexcept;
    void set_live_anchor(uint32_t on) noexcept;
    LiveSettings live_settings() const noexcept;
    static std::wstring find_runtime_dir();
    static std::wstring find_pushflow_dir(PushFlowTier tier);
};
namespace detail {
struct WriteEventStatus {
    bool opened{};
    bool pended{};
    bool cancelled{};
    bool aborted{};
    bool deferredToPool{};
    bool timedOut{};
};
struct WriteTestHooks {
    HANDLE reapedEvent{};
    HANDLE stallBeforeIoEvent{};
    bool failPoolPrep{};
    bool skipCancelIoEx{};
};
WriteEventStatus write_event_bounded(const std::wstring& path, const std::string& text,
    DWORD timeoutMs = 150, const WriteTestHooks* hooks = nullptr) noexcept;
using MarkerMutexCreateFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
void set_marker_mutex_create_fn_for_test(MarkerMutexCreateFn fn) noexcept;
}
} // namespace nr::fg::vfg
