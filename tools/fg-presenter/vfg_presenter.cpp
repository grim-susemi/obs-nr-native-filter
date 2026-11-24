// SPDX-License-Identifier: GPL-3.0-only
#include "native_vfg.h"
#include "anr_fg_backend.h"
#include "pushflow_backend.h"
#include "vfg_adaptive_fg.h"
#include "vfg_cadence.h"
#include "vfg_generation_scope.h"
#include "vfg_hud.h"
#include "vfg_pacing_repair.h"
#include "vfg_reflex.h"
#include <mmsystem.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <io.h>
#include <intrin.h>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace nr::fg::vfg {
namespace {
LONG64 state(Session& view) { return InterlockedCompareExchange64(&view.state, 0, 0); }

struct MonitorPick {
    wchar_t wantedId[128]{};
    HMONITOR handle{};
    MONITORINFOEXW info{};
    unsigned matches{};
    bool cloned{};
    HMONITOR fallbackNonPrimary{};
    MONITORINFOEXW fallbackNonPrimaryInfo{};
    HMONITOR fallbackPrimary{};
    MONITORINFOEXW fallbackPrimaryInfo{};
};

BOOL CALLBACK enum_monitors(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto& target = *reinterpret_cast<MonitorPick*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return FALSE;
    const bool isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (isPrimary && !target.fallbackPrimary) {
        target.fallbackPrimary = monitor;
        target.fallbackPrimaryInfo = info;
    } else if (!isPrimary && !target.fallbackNonPrimary) {
        target.fallbackNonPrimary = monitor;
        target.fallbackNonPrimaryInfo = info;
    }
    if (target.wantedId[0]) {
        unsigned active = 0, matches = 0;
        for (DWORD index = 0;; ++index) {
            DISPLAY_DEVICEW device{};
            device.cb = sizeof(device);
            if (!EnumDisplayDevicesW(info.szDevice, index, &device, EDD_GET_DEVICE_INTERFACE_NAME)) break;
            if (!(device.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
            ++active;
            if (_wcsicmp(device.DeviceID, target.wantedId) == 0) ++matches;
        }
        if (matches) {
            target.matches += matches;
            target.cloned |= active != 1;
            target.handle = monitor;
            target.info = info;
        }
    }
    return TRUE;
}

constexpr uint64_t ms = 1000000;
constexpr UINT reveal = WM_APP + 1;
constexpr UINT syncAffinity = WM_APP + 2;
constexpr UINT workerFinished = WM_APP + 3;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

struct Feed {
    Session& view;
    HANDLE parent;
    HWND window;
    FILE* log;
    const wchar_t* runtime;
    uint32_t cliMultiplier;
    uint32_t hudScale;
    struct SplitState& split;
    std::atomic_uint64_t heartbeat{clock_ns()};
    std::atomic_int result{-1};
    std::atomic_bool initializing{true};
    std::atomic_bool userClosing{false};
    std::atomic_bool pushflowActive{false};
    pushflow::PreviewMultiplier previewMultiplier;
    bool stopped() { return state(view) < 0 || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT; }
    bool active() { return state(view) == 1 && !stopped(); }
    void beat() { heartbeat.store(clock_ns()); }
};

struct SplitState {
    std::mutex mutex;
    Session* session{};
    std::atomic_uint64_t displayEpoch{};
    std::atomic_bool* userClosing{};
    bool enabled{}, dragging{}, initialized{}, overridden{};
    uint32_t observed{50}, percent{50};

    void reset(bool on) {
        std::lock_guard lock(mutex);
        enabled = on;
        dragging = initialized = overridden = false;
        observed = percent = 50;
    }
    uint32_t update(uint32_t value) {
        std::lock_guard lock(mutex);
        value = std::min(value, 100u);
        if (!initialized || value != observed) {
            observed = value;
            initialized = true;
            overridden = false;
        }
        if (!overridden) percent = observed;
        return percent;
    }
    uint32_t current() {
        std::lock_guard lock(mutex);
        return percent;
    }
};

struct FaultGuard {
    Session& view;
    ~FaultGuard() { if (std::uncaught_exceptions() > 0) stamp_exit_reason(view, ExitReason::Fault); }
};

DWORD fixture_ms(const Session& s, const wchar_t* name)
{
    if (s.launchEpoch != 1 && wcscmp(name, L"OBS_NR_TEST_STALL_TEARDOWN_MS")) return 0;
    wchar_t value[8]{};
    const auto n = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    if (!n || n >= ARRAYSIZE(value)) return 0;
    wchar_t* end{};
    const auto delay = wcstoul(value, &end, 10);
    return *end ? 0 : std::min(delay, 30'000ul);
}

void clear_pushflow_starting_on_close(const Feed& f) noexcept
{
    try {
        if (pushflow::clear_starting_on_user_close(pushflow::marker_directory(), f.pushflowActive.load(),
            read_exit_reason(f.view))) return;
    } catch (...) {
    }
    fprintf(f.log, "event=pushflow_marker_cleanup_failed\n");
}

double window_refresh(HWND window) noexcept
{
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return 0;
    return query_monitor_refresh_hz(monitor.szDevice);
}

struct PairTelemetry {
    uint32_t multiplier{1};
    int64_t shiftNs{};
    bool hudLock{}, shotCut{}, pacingRepair{}, adaptiveFg{};
    LONG adaptiveTargetFps{};
};

void record(Feed& f, const char* kind, const ipc::Frame& b, const ipc::Frame* a, UINT id,
    const PairTelemetry& telemetry, uint64_t workNs = 0, uint64_t eventNs = 0)
{
    const auto q = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    fprintf(f.log, "event=%s generation=%llu b=%llu b_qpc_ns=%llu b_arrival_ns=%llu b_obs_tick=%llu a=%llu a_qpc_ns=%llu present_id=%u qpc_ns=%llu work_ns=%llu eff_mult=%u pacing_shift_ns=%lld hud_lock=%d shot_cut=%d pacing_repair=%d adaptive_fg=%d adaptive_target_fps=%ld\n",
        kind, q(b.generation), q(b.serial), q(b.timestampNs), q(b.arrivalNs), q(b.obsTick), q(a ? a->serial : 0),
        q(a ? a->timestampNs : 0), id, q(eventNs ? eventNs : clock_ns()), q(workNs), telemetry.multiplier,
        static_cast<long long>(telemetry.shiftNs), telemetry.hudLock, telemetry.shotCut,
        telemetry.pacingRepair, telemetry.adaptiveFg, telemetry.adaptiveTargetFps);
}

void pace_until(uint64_t targetNs, Feed& f, HANDLE timer)
{
    for (;;) {
        if (!f.active()) return;
        const uint64_t now = clock_ns();
        if (now >= targetNs) return;
        const auto remaining = targetNs - now;
        if (remaining <= 300000) { YieldProcessor(); continue; }
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>((remaining - 200000) / 100);
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            if (WaitForSingleObject(timer, static_cast<DWORD>(remaining / ms + 2)) != WAIT_FAILED)
                continue;
        }
        if (remaining >= ms) Sleep(1);
        else SwitchToThread();
    }
}

struct TimerPeriodGuard {
    bool active{};
    TimerPeriodGuard() noexcept : active(timeBeginPeriod(1) == TIMERR_NOERROR) {}
    ~TimerPeriodGuard() { if (active) timeEndPeriod(1); }
};

void frames(Feed& f)
{
    FaultGuard outerFault{f.view};
    TimerPeriodGuard timerRes;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (!f.active()) return;
    f.beat();
    f.initializing.store(true);
    Handle compareReady, paceTimer;
    compareReady.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!compareReady.h) check(HRESULT_FROM_WIN32(GetLastError()), "comparison event");
    paceTimer.h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (f.view.width < 320 || f.view.height < 180 || f.view.width > 7680 || f.view.height > 4320 ||
        !f.view.generation || !f.view.mapping[0])
        check(E_INVALIDARG, "unsupported session offer");
    const auto requested = f.cliMultiplier <= 1 ? 1u :
        std::clamp<uint32_t>(f.view.multiplier ? f.view.multiplier : f.cliMultiplier, 2, max_multiplier);
    auto mult = f.previewMultiplier.effective(requested);
    fprintf(f.log, "event=offer mode=%ux size=%ux%u quality=%u generation=%llu luid=%08lx:%08lx hwnd=%p pid=%lu backend=%s epoch=%llu\n",
        mult, f.view.width, f.view.height, f.view.quality,
        static_cast<unsigned long long>(f.view.generation), static_cast<unsigned long>(f.view.adapter.HighPart),
        f.view.adapter.LowPart, static_cast<void*>(f.window), GetCurrentProcessId(), backend_name(f.view.backend),
        static_cast<unsigned long long>(f.view.launchEpoch));
    set_gpu_process_priority(InterlockedCompareExchange(&f.view.gpuPriority, 0, 0) != 0, f.log);
    const bool split = f.view.splitCompare != 0;
    f.split.reset(split);
    Gpu gpu(f.view, f.window, mult);
    ReflexContext reflex(f.view.backend == Backend::NativeVfg ? gpu.d3d_device() : nullptr,
        f.view.backend == Backend::NativeVfg ? gpu.drawQueue.Get() : nullptr, f.log);
    LONG lastReflex = -1;
    auto sync_reflex = [&]() {
        const LONG want = InterlockedCompareExchange(&f.view.lowLatency, 0, 0) ? 1 : 0;
        if (want != lastReflex) {
            reflex.set_sleep_mode(want != 0);
            lastReflex = want;
        }
    };
    sync_reflex();
    LONG lastHide = InterlockedCompareExchange(&f.view.hideFromCapture, 0, 0) ? 1 : 0;
    auto sync_affinity = [&]() {
        const LONG want = InterlockedCompareExchange(&f.view.hideFromCapture, 0, 0) ? 1 : 0;
        if (want != lastHide) {
            lastHide = want;
            const DWORD affinity = display_affinity(want);
            PostMessageW(f.window, syncAffinity, static_cast<WPARAM>(want), 0);
            fprintf(f.log, "event=display_affinity affinity=0x%lX\n", affinity);
        }
    };
    if (!f.active()) return;
    ipc::Consumer consumer(gpu.ipcQueue.Get(), {f.view.mapping, f.view.generation}, ipc::identity(f.parent));
    FaultGuard innerFault{f.view};
    std::unique_ptr<IFgBackend> backend;
    PushFlowBackend* pushflowBackend{};
    AnrFgBackend* anrBackend{};
    IFgBackend* vfg{};
    const auto userClosing = [&f] { return read_exit_reason(f.view) == ExitReason::UserClosed; };
    const auto drain_failure = [&] {
        stamp_exit_reason(f.view, ExitReason::Fault);
        const bool userClosed = f.view.backend == Backend::PushFlow &&
            read_exit_reason(f.view) == ExitReason::UserClosed;
        if (userClosed) clear_pushflow_starting_on_close(f);
        fprintf(f.log, "event=error message=presenter_drain_failed\n");
        fflush(f.log);
        ShowWindowAsync(f.window, SW_HIDE);
        TerminateProcess(GetCurrentProcess(), userClosed ? 0u : 5u);
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    };
    const auto work = [&] {
    // External frame providers keep their owners through generation shutdown.
    // Native work exceptions still propagate directly to the worker.
    struct HudUnwindGuard {
        const Gpu& gpu;
        const decltype(drain_failure)& fail;
        bool preserveExternalOwner;
        ~HudUnwindGuard()
        {
            if (!preserveExternalOwner && std::uncaught_exceptions() > 0 && !gpu.drain_hud_on_unwind()) fail();
        }
    } hudUnwind{gpu, drain_failure, f.view.backend != Backend::NativeVfg};
    if (mult > 1) {
        switch (f.view.backend) {
        case Backend::NativeVfg:
            backend = std::make_unique<NativeVfg>(f.runtime, gpu, f.view, f.view.gpuShotChange != 0, [&f] { f.beat(); });
            vfg = backend.get();
            break;
        case Backend::PushFlow: {
            if (!f.view.pushflowRuntimeDir[0]) {
                fprintf(f.log, "event=pushflow_runtime_missing_fallback_1x\n");
                break;
            }
            auto owner = std::make_unique<PushFlowBackend>();
            pushflowBackend = owner.get();
            backend = std::move(owner);
            f.pushflowActive.store(true);
            if (pushflowBackend->open({f.view, f.log, [&f] { f.beat(); }, userClosing, 0, &f.previewMultiplier}, &gpu))
                vfg = pushflowBackend;
            else if (pushflowBackend->runtime_fault())
                check(E_FAIL, "PushFlow initialization fault");
            else
                f.pushflowActive.store(false);
            break;
        }
        case Backend::AnrExperimental: {
            auto owner = std::make_unique<AnrFgBackend>();
            anrBackend = owner.get();
            backend = std::move(owner);
            if (anrBackend->open({gpu.d3d_device(), gpu.drawQueue.Get(), {f.view.width, f.view.height},
                    f.view.generation, f.view.anrDllPath, f.view.experimentalAnrOptIn == 1, f.log, &gpu}))
                vfg = anrBackend;
            mult = std::min(mult, anrBackend->multiplier_limit());
            break;
        }
        }
    }
    fprintf(f.log, "event=initialized backend=%s sdk_version=0x%08X low_latency=%ld gpu_shot_change=%u hud_lock=%ld pacing_repair=%ld adaptive_fg=%ld\n",
        vfg ? vfg->name() : "original", vfg ? vfg->version_tag() : 0u, lastReflex, f.view.gpuShotChange,
        InterlockedCompareExchange(&f.view.hudLock, 0, 0), InterlockedCompareExchange(&f.view.pacingRepair, 0, 0),
        InterlockedCompareExchange(&f.view.adaptiveFg, 0, 0));
    f.beat();
    f.initializing.store(false);
    if (const auto stall = fixture_ms(f.view, L"OBS_NR_TEST_STALL_WORKER_MS")) {
        fprintf(f.log, "event=test_worker_stalled delay_ms=%lu\n", stall);
        Sleep(stall);
    }
    const auto faultExitDelay = fixture_ms(f.view, L"OBS_NR_TEST_FAULT_EXIT_DELAY_MS");
    Handle faultArm;
    if (faultExitDelay) {
        wchar_t name[128]{};
        const auto size = GetEnvironmentVariableW(L"OBS_NR_TEST_FAULT_ARM_EVENT", name, ARRAYSIZE(name));
        if (size && size < ARRAYSIZE(name)) {
            faultArm.h = OpenEventW(SYNCHRONIZE, FALSE, name);
            if (!faultArm.h) check(E_FAIL, "test fault arm event");
        }
    }
    bool shown = false;
    ContentHistory history;
    UniqueCadence cadence;
    uint64_t lastOriginalTarget{};
    uint64_t lastPeriod{};
    bool sawRepeat = false;
    PushPacingRepair repair;
    TransportIntervalEstimator transport;
    ContentRateTracker rate;
    AdaptiveMultiplierGate multiplierGate;
    uint64_t previousRepairedPtsNs{}, uniqueOrdinal{}, lastChangedNs{}, previousObsTick{};
    int64_t transportNs = TransportIntervalEstimator::kDefaultNs;
    bool staticHold = false, wasRepairing = false, wasAdaptive = false, backendFault = false;
    bool hudEnabled = false, hudReset = true;
    const auto sync_hud_lock = [&] {
        const bool enabled = InterlockedCompareExchange(&f.view.hudLock, 0, 0) != 0;
        if (enabled != hudEnabled) hudReset = true;
        hudEnabled = enabled;
        gpu.sync_hud_lock(enabled);
    };
    double contentFps = 0;
    auto displayEpoch = f.split.displayEpoch.load();
    auto monitorHz = window_refresh(f.window);
    std::optional<FgPairTicket> previousTicket;
    uint64_t hudWindowStart = clock_ns();
    uint64_t hudOrigFrames = 0, hudPresentedFrames = 0;
    uint32_t lastHudPos = UINT32_MAX;
    std::string hudText = "-- FPS", lastHudText;
    auto refresh_hud = [&](bool isOriginal) {
        ++hudPresentedFrames;
        if (isOriginal) ++hudOrigFrames;
        const uint64_t now = clock_ns();
        const uint64_t dt = now - hudWindowStart;
        const uint32_t pos = static_cast<uint32_t>(std::clamp<LONG>(InterlockedCompareExchange(&f.view.fpsOverlay, 0, 0), 0, 2));
        if (dt >= 1000 * ms) {
            const double sec = static_cast<double>(dt) / 1e9;
            hudText = hud::label(static_cast<uint32_t>(hudOrigFrames / sec + 0.5),
                static_cast<uint32_t>(hudPresentedFrames / sec + 0.5));
            hudOrigFrames = 0;
            hudPresentedFrames = 0;
            hudWindowStart = now;
        }
        if (pos == 0) {
            if (lastHudPos != 0) { gpu.set_hud(0, 0, 0, 0, nullptr); lastHudPos = 0; }
            return;
        }
        if (pos != lastHudPos || hudText != lastHudText) {
            const auto img = hud::raster(hudText, f.hudScale);
            gpu.set_hud(pos, hud::inset * f.hudScale, img.width, img.height, img.pixels.data());
            lastHudPos = pos;
            lastHudText = hudText;
        }
    };
    struct Incoming {
        ipc::Frame frame;
        PairTelemetry telemetry;
        uint64_t receivedNs;
        unsigned index;
        bool continuous;
        bool interpolate;
        uint64_t compareStart{};
    };
    std::optional<Incoming> incoming;
    uint64_t reflexFrameId = 0;
    while (f.active() && !(pushflowBackend && pushflowBackend->runtime_fault())) {
        if (faultArm.h && WaitForSingleObject(faultArm.h, 0) == WAIT_OBJECT_0)
            throw std::runtime_error("test_live_consumer_fault");
        sync_reflex();
        sync_affinity();
        sync_hud_lock();
        if (!incoming) {
            const auto index = history.input_index();
            gpu.prepare_input(index);
            const auto next = consumer.copy_next(gpu.input[index].Get(), split ? gpu.before[index].Get() : nullptr);
            if (!next.current) {
                if (next.status != ipc::Status::Pending && next.status != ipc::Status::Dropped)
                    check(E_FAIL, "IPC copy_next");
                f.beat();
                WaitForSingleObject(consumer.frame_event(), 4);
                continue;
            }
            if (next.status != ipc::Status::Ok || consumer.order_copy(*next.current, gpu.drawQueue.Get()) != ipc::Status::Ok)
                check(E_FAIL, "IPC copy dependency");
            const auto& b = next.current->frame;
            if (split) f.split.update(b.splitPercent);
            const auto receivedNs = clock_ns();
            if (vfg && vfg->faulted() && !backendFault) {
                backendFault = true; // Keep resources alive until their queues drain.
                fprintf(f.log, "event=backend_fault_fallback_1x backend=%s\n", vfg->name());
            }
            const bool interpolate = vfg && !backendFault;
            const PairTelemetry telemetry{1, 0, hudEnabled,
                false, InterlockedCompareExchange(&f.view.pacingRepair, 0, 0) != 0,
                InterlockedCompareExchange(&f.view.adaptiveFg, 0, 0) != 0,
                InterlockedCompareExchange(&f.view.adaptiveTargetFps, 0, 0)};
            const auto step = history.observe(index, b, next.previous.has_value());
            if (step.stale) {
                record(f, "input", b, nullptr, 0, telemetry, 0, receivedNs);
                record(f, "drop_original_stale", b, nullptr, 0, telemetry);
                continue;
            }
            if (!step.continuous) {
                hudReset = true;
                cadence.reset();
                lastOriginalTarget = lastPeriod = 0;
                sawRepeat = false;
                repair.reset();
                rate.reset();
                staticHold = false;
                previousRepairedPtsNs = lastChangedNs = 0;
            }
            if (b.obsTick > previousObsTick && b.obsTick <= INT64_MAX) {
                transportNs = transport.observe(static_cast<int64_t>(b.obsTick));
                previousObsTick = b.obsTick;
            }
            incoming = Incoming{b, telemetry, receivedNs, index, step.continuous, interpolate};
        }
        const auto b = incoming->frame;
        const auto index = incoming->index;
        const auto continuous = incoming->continuous;
        const auto anchor = history.anchor();
        auto telemetry = incoming->telemetry;
        telemetry.hudLock &= hudEnabled;
        const bool adaptive = telemetry.adaptiveFg, repairing = telemetry.pacingRepair;
        const bool interpolate = incoming->interpolate;
        uint64_t comparedNs{}, compareWorkNs{};
        bool shotCompared = false;
        uint32_t shotParity = 0;
        const auto record_input = [&](const char* comparison = "content_changed") {
            // Publish the ingress timestamp with the decision completed for this
            // frame. A comparison retry must not re-read mutable settings.
            record(f, "input", b, nullptr, 0, telemetry, 0, incoming->receivedNs);
            if (comparedNs) record(f, comparison, b, &anchor->frame, 0, telemetry, compareWorkNs, comparedNs);
            incoming.reset();
        };
        const bool timely = clock_ns() - b.arrivalNs <= 50 * ms;
        if ((interpolate || telemetry.hudLock) && anchor && timely) {
            if (!incoming->compareStart) {
                if (!gpu.begin_compare(anchor->index, index, compareReady.h)) {
                    pace_until(clock_ns() + 350000, f, paceTimer.h);
                    continue;
                }
                incoming->compareStart = clock_ns();
            }
            const auto compared = gpu.compare_result();
            if (!compared) {
                // The completion event wakes the presentation loop. This branch
                // does not map the comparison buffer or block a submit call.
                const HANDLE wake[]{f.parent, compareReady.h};
                WaitForMultipleObjects(ARRAYSIZE(wake), wake, FALSE, 2);
                continue;
            }
            comparedNs = clock_ns();
            compareWorkNs = comparedNs - incoming->compareStart;
            shotCompared = f.view.gpuShotChange != 0;
            shotParity = compared->parity;
            telemetry.shotCut = compared->cut;
            if (compared->same) {
                // A single transport repeat is not a static hold. Once stillness
                // reaches 500 ms, retain the classification through repeats.
                if (lastChangedNs && b.timestampNs >= lastChangedNs && b.timestampNs - lastChangedNs >= 500 * ms)
                    staticHold = true;
                contentFps = rate.observe(b.timestampNs, uniqueOrdinal, staticHold);
                sawRepeat = true;
                record_input("repeat_input");
                present_content_repeat(interpolate, split, [&] {
                    if (!shown) {
                        PostMessageW(f.window, reveal, 0, 0);
                        shown = true;
                    }
                    refresh_hud(true);
                    const UINT pid = gpu.present(gpu.input[index].Get(), false,
                        split ? static_cast<int>(index) : -1, split ? f.split.current() : 50);
                    record(f, split ? "present_repeat_split" : "present_repeat_original", b, nullptr, pid, telemetry);
                });
                f.beat();
                continue;
            }
        } else if (incoming->compareStart) {
            // OFF may no longer need a HUD-only result; an expired frame never does.
            gpu.discard_compare();
        }
        if (clock_ns() - b.arrivalNs > 50 * ms) {
            record_input();
            record(f, "drop_original_late", b, nullptr, 0, telemetry);
            history.invalidate();
            hudReset = true;
            cadence.reset();
            lastOriginalTarget = lastPeriod = 0;
            sawRepeat = false;
            previousRepairedPtsNs = 0;
            continue;
        }
        staticHold = false;
        lastChangedNs = b.timestampNs;
        if (repairing != wasRepairing) {
            cadence.reset();
            repair.reset();
            previousRepairedPtsNs = lastOriginalTarget = lastPeriod = 0;
            wasRepairing = repairing;
        }
        auto timestamp = b.obsTick ? b.obsTick : b.timestampNs;
        auto phaseArrivalNs = b.arrivalNs;
        if (repairing) {
            if (timestamp > INT64_MAX || phaseArrivalNs > INT64_MAX) { record_input(); continue; }
            const auto raw = static_cast<int64_t>(timestamp);
            const auto result = repair.apply(raw / 100, raw / 100, transportNs / 100, !continuous);
            if (result.disposition == PushPacingRepair::Disposition::noRepresentableProgress) { record_input(); continue; }
            const auto repaired = repaired_times(raw, static_cast<int64_t>(phaseArrivalNs), result);
            if (!repaired || repaired->repairedContentPtsNs < 0 || repaired->phaseArrivalNs < 0) { record_input(); continue; }
            timestamp = static_cast<uint64_t>(repaired->repairedContentPtsNs);
            telemetry.shiftNs = result.shift * 100;
            phaseArrivalNs = static_cast<uint64_t>(repaired->phaseArrivalNs);
        }
        telemetry.multiplier = interpolate ? mult : 1u;
        contentFps = rate.observe(b.timestampNs, ++uniqueOrdinal, staticHold);
        auto period = interpolate ? cadence.observe(timestamp) : 0;
        const auto previous = repairing ? previousRepairedPtsNs :
            anchor ? (anchor->frame.obsTick ? anchor->frame.obsTick : anchor->frame.timestampNs) : 0;
        if (!period && interpolate && anchor && (repairing || (b.obsTick != 0) == (anchor->frame.obsTick != 0)) &&
            previous && provisional_60hz(previous, timestamp, sawRepeat))
            period = period_60hz;
        previousRepairedPtsNs = repairing ? timestamp : 0;
        if (adaptive != wasAdaptive) {
            multiplierGate.reset();
            wasAdaptive = adaptive;
        }
        if (adaptive && interpolate) {
            if (const auto epoch = f.split.displayEpoch.load(); epoch != displayEpoch) {
                displayEpoch = epoch;
                monitorHz = window_refresh(f.window);
            }
            const auto requested = telemetry.adaptiveTargetFps;
            const auto selected = adaptive_multiplier(static_cast<AdaptiveBackend>(f.view.backend), mult,
                contentFps, requested > 1 ? static_cast<double>(requested) : monitorHz);
            telemetry.multiplier = multiplierGate.update(selected, static_cast<int64_t>(b.timestampNs / 100));
            if (contentFps > 0)
                period = static_cast<uint64_t>(std::clamp(1e9 / contentFps, 8'333'333.0, 50'000'000.0) + 0.5);
        }
        if (!adaptive && period != lastPeriod) lastOriginalTarget = 0;
        lastPeriod = period;
        const auto activeMult = telemetry.multiplier;
        const auto plan = !anchor || !period ? CadencePlan{} : adaptive ?
            plan_cadence_adaptive(phaseArrivalNs, lastOriginalTarget, period, activeMult) :
            plan_cadence(phaseArrivalNs, lastOriginalTarget, period, activeMult);
        record_input();
        if (telemetry.hudLock) {
            const bool reset = hudReset || telemetry.shotCut || !anchor;
            gpu.update_hud_lock(index, anchor ? anchor->index : index, reset);
            hudReset = false;
        }
        const auto originalDeadline = plan.originalNs ? plan.originalNs + 5 * ms : b.arrivalNs + 50 * ms;
        const uint64_t fid = ++reflexFrameId;
        reflex.sleep(clock_ns(), clock_ns);
        reflex.marker(ReflexMarker::SimulationStart, fid);
        reflex.marker(ReflexMarker::SimulationEnd, fid);
        reflex.marker(ReflexMarker::RenderSubmitStart, fid);
        if (interpolate && anchor && plan.originalNs) {
            const auto& a = anchor->frame;
            const bool backendBusy = previousTicket && (anrBackend ? !anrBackend->pair_complete(*previousTicket) :
                !vfg->subframe_ready(*previousTicket, previousTicket->lastSubmittedSub));
            const auto stageStart = clock_ns();
            const bool pairLate = stageStart >= plan.synthetic_cutoff(activeMult - 1);
            const FgPairDesc pair{gpu.input[anchor->index].Get(), gpu.input[index].Get(), a.serial, b.serial, activeMult,
                !continuous, telemetry.shotCut,
                [&] { record(f, "stage_queued", b, &a, 0, telemetry, clock_ns() - stageStart); },
                [&](uint64_t workNs) { record(f, pushflowBackend ? "pushflow_enqueued" : "cuda_enqueued",
                    b, &a, 0, telemetry, workNs); }};
            const auto ticket = !backendBusy && !pairLate ?
                vfg->try_submit_pair(pair, anchor->index, index) : std::nullopt;
            if (ticket) {
                previousTicket.emplace(*ticket);
                reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
                bool composeHud = telemetry.hudLock;
                for (uint32_t sub = 1; sub <= ticket->lastSubmittedSub; ++sub) {
                    if (!f.active()) break;
                    const uint64_t targetNs = plan.target(sub);
                    if (clock_ns() < targetNs) pace_until(targetNs, f, paceTimer.h);
                    if (!f.active()) break;
                    const uint64_t waitStart = clock_ns();
                    const auto cutoff = plan.synthetic_cutoff(sub);
                    bool generatedReady = false;
                    bool providerSuppressed = false;
                    while (f.active() && clock_ns() < cutoff) {
                        if (anrBackend) {
                            switch (anrBackend->output(*ticket, sub)) {
                            case AnrFgBackend::Output::Ready: generatedReady = true; break;
                            case AnrFgBackend::Output::Suppressed: providerSuppressed = true; break;
                            case AnrFgBackend::Output::Pending: break;
                            }
                            if (generatedReady || providerSuppressed) break;
                        } else if (vfg->subframe_ready(*ticket, sub)) {
                            generatedReady = true;
                            break;
                        }
                        pace_until(std::min<uint64_t>(clock_ns() + 350000, cutoff), f, paceTimer.h);
                    }
                    const auto readyCheck = clock_ns();
                    const uint64_t genElapsed = readyCheck - waitStart;
                    if (!f.active()) break;
                    if (generatedReady && readyCheck < cutoff) {
                        sync_hud_lock();
                        composeHud &= hudEnabled;
                        refresh_hud(false);
                        reflex.oob_marker(ReflexMarker::OutOfBandRenderSubmitStart, fid, sub);
                        reflex.oob_marker(ReflexMarker::OutOfBandRenderSubmitEnd, fid, sub);
                        reflex.oob_marker(ReflexMarker::OutOfBandPresentStart, fid, sub);
                        auto* const displayTex = gpu.syntheticTex[sub - 1].Get();
                        const Gpu::SyntheticFrame synthetic{displayTex, [&](ID3D12GraphicsCommandList* cmd) {
                            vfg->resolve_subframe(cmd, *ticket, displayTex, sub);
                        }, ticket->prevIndex, ticket->currIndex, shotCompared, shotParity, composeHud, sub - 1};
                        const auto pid = gpu.try_present_synthetic(synthetic, cutoff,
                            split ? static_cast<int>(index) : -1, split ? f.split.current() : 50);
                        reflex.oob_marker(ReflexMarker::OutOfBandPresentEnd, fid, sub);
                        record(f, pid ? "present_vfg_subframe" : "drop_vfg_present_busy", b, &a,
                            pid.value_or(0), telemetry, genElapsed);
                    } else {
                        record(f, providerSuppressed ? "drop_vfg_provider_disabled" : "drop_vfg_subframe_late",
                            b, &a, 0, telemetry,
                            readyCheck > cutoff ? readyCheck - cutoff : 0);
                    }
                }
            } else {
                reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
                if (anrBackend && anrBackend->faulted()) telemetry.multiplier = 1;
                record(f, backendBusy ? "drop_vfg_gpu_busy" :
                    pairLate ? "drop_vfg_pair_late" : "drop_vfg_stage_busy", b, &a, 0, telemetry);
            }
        } else {
            reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
            if (vfg && !continuous)
                record(f, "drop_vfg_discontinuity", b, nullptr, 0, telemetry);
        }
        if (plan.originalNs) pace_until(plan.originalNs, f, paceTimer.h);
        if (!f.active()) break;
        if (clock_ns() <= originalDeadline) {
            if (!shown) {
                PostMessageW(f.window, reveal, 0, 0);
                shown = true;
            }
            refresh_hud(true);
            reflex.marker(ReflexMarker::PresentStart, fid);
            const UINT pid = gpu.present(gpu.input[index].Get(), false,
                split ? static_cast<int>(index) : -1, split ? f.split.current() : 50);
            reflex.marker(ReflexMarker::PresentEnd, fid);
            record(f, "present_original", b, nullptr, pid, telemetry);
            history.retain(index, b);
            gpu.enter_steady_state();
            lastOriginalTarget = plan.originalNs;
            f.beat();
            if (faultExitDelay && !faultArm.h)
                throw std::runtime_error("test_live_consumer_fault");
        } else {
            record(f, "drop_original_late", b, nullptr, 0, telemetry);
            history.invalidate();
            hudReset = true;
            cadence.reset();
            lastOriginalTarget = lastPeriod = 0;
            sawRepeat = false;
            previousRepairedPtsNs = 0;
        }
    }
    if (incoming) record(f, "input", incoming->frame, nullptr, 0, incoming->telemetry, 0, incoming->receivedNs);
    };
    const auto shutdown = [&](bool threw) {
    gpu.discard_compare();
    if (threw && pushflowBackend) pushflowBackend->generation_fault();
    PostMessageW(f.window, WM_CANCELMODE, 0, 0);
    // Both queues must be idle before releasing their resources or acknowledging
    // a resize. In particular, a successful Present only submits draw work.
    ComPtr<ID3D12Fence> drainFence;
    if (FAILED(gpu.d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drainFence))))
        drain_failure();
    Handle drainEvent;
    drainEvent.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!drainEvent.h) drain_failure();
    const auto drain = [&](ID3D12CommandQueue* queue, UINT64 value) {
        if (FAILED(queue->Signal(drainFence.Get(), value)) ||
            FAILED(drainFence->SetEventOnCompletion(value, drainEvent.h))) drain_failure();
        if (WaitForSingleObject(drainEvent.h, 240) != WAIT_OBJECT_0)
            drain_failure();
        const auto completed = drainFence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < value)
            drain_failure();
    };
    try {
        if (pushflowBackend) pushflowBackend->synchronize();
        drain(gpu.ipcQueue.Get(), 1);
        drain(gpu.drawQueue.Get(), 2);
        if (vfg && !pushflowBackend && !anrBackend) vfg->synchronize(); // Do not acknowledge a failed CUDA drain.
    } catch (const std::exception& error) {
        if (f.view.backend != Backend::PushFlow)
            fprintf(f.log, "event=error message=%s\n", error.what());
        drain_failure();
    } catch (...) {
        drain_failure();
    }
    if (const auto stall = fixture_ms(f.view, L"OBS_NR_TEST_STALL_TEARDOWN_MS")) {
        fprintf(f.log, "event=test_teardown_stalled delay_ms=%lu\n", stall);
        Sleep(stall);
    }
    const bool pushflowFault = pushflowBackend && pushflowBackend->runtime_fault();
    try {
        if (pushflowBackend && !pushflowBackend->close()) drain_failure();
        if (anrBackend && !anrBackend->close()) drain_failure();
    } catch (...) {
        drain_failure();
    }
    backend.reset();
    clear_pushflow_starting_on_close(f);
    f.pushflowActive.store(false);
    if (consumer.close() != ipc::Status::Closed) {
        if (f.view.backend == Backend::PushFlow) drain_failure();
        check(E_FAIL, "IPC drain incomplete");
    }
    f.split.reset(false);
    const bool closed = userClosing();
    if (pushflowFault && !threw && !closed)
        check(E_FAIL, "PushFlow inference fault");
    return closed && f.view.backend == Backend::PushFlow;
    };
    run_generation(work, shutdown, f.view.backend != Backend::NativeVfg);
}
} // namespace
} // namespace nr::fg::vfg

int wmain(int argc, wchar_t** argv)
{
    using namespace nr::fg::vfg;
    uint32_t cliMult = 0;
    if (argc == 6) {
        if (wcscmp(argv[5], L"off") == 0) cliMult = 1;
        else if (wcscmp(argv[5], L"2x") == 0) cliMult = 2;
        else if (wcscmp(argv[5], L"3x") == 0) cliMult = 3;
        else if (wcscmp(argv[5], L"4x") == 0) cliMult = 4;
    }
    if (!cliMult) return 2;

    const auto handle = [](const wchar_t* s) {
        wchar_t* end{};
        const auto n = wcstoull(s, &end, 10);
        return n && !*end ? reinterpret_cast<HANDLE>(static_cast<uintptr_t>(n)) : nullptr;
    };
    Handle parent, mapping;
    parent.h = handle(argv[1]);
    mapping.h = handle(argv[2]);
    if (!parent.h || !mapping.h) return 2;
    auto* view = static_cast<Session*>(MapViewOfFile(mapping.h, FILE_MAP_WRITE, 0, 0, sizeof(Session)));
    if (!view) return 2;
    struct View { Session* p; ~View() { UnmapViewOfFile(p); } } map{view};

    const HANDLE logHandle = CreateFileW(argv[4], FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) return 2;
    const int logFd = _open_osfhandle(reinterpret_cast<intptr_t>(logHandle), _O_APPEND | _O_BINARY);
    if (logFd == -1) { CloseHandle(logHandle); return 2; }
    FILE* file = _fdopen(logFd, "ab");
    if (!file) { _close(logFd); return 2; }
    std::unique_ptr<FILE, decltype(&fclose)> log(file, &fclose);
    setvbuf(file, nullptr, _IONBF, 0);
    if (!validate_session_header(*view)) {
        fprintf(file, "event=error message=unsupported_session_version\n");
        return 2;
    }
    if (view->launchEpoch != 1 || view->backend == Backend::NativeVfg)
        SetEnvironmentVariableW(L"OBS_NR_TEST_REJECT_FIRST_IMPORT", nullptr);
    fprintf(file, "event=session_started backend=%s pid=%lu epoch=%llu\n", backend_name(view->backend),
        GetCurrentProcessId(), static_cast<unsigned long long>(view->launchEpoch));

    // Wait briefly for offer() if monitorId is supplied through Session
    while (state(*view) == 0 && WaitForSingleObject(parent.h, 0) == WAIT_TIMEOUT) Sleep(2);
    if (state(*view) < 0) return 0;

    if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        // Ignore if already set
    }
    MonitorPick target{};
#ifdef OBS_NR_VFG_QA_ACTION
    const auto reject = [&](const char* reason) {
        fprintf(file, "event=qa_monitor_rejected reason=%s\n", reason);
        return 2;
    };
    const DWORD len = GetEnvironmentVariableW(L"OBS_NR_VFG_QA_MONITOR", target.wantedId, ARRAYSIZE(target.wantedId));
    if (!len || len >= ARRAYSIZE(target.wantedId)) return reject("missing_or_invalid_selector");
    if (!EnumDisplayMonitors(nullptr, nullptr, enum_monitors, reinterpret_cast<LPARAM>(&target))) return reject("enumeration_failed");
    if (!target.matches) return reject("no_active_match");
    if (target.matches != 1 || target.cloned) return reject("ambiguous_or_cloned");
    if (target.info.dwFlags & MONITORINFOF_PRIMARY) return reject("primary_target");
    RECT bounds = target.info.rcMonitor;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return reject("invalid_rectangle");
#else
    if (view->monitorId[0]) wcscpy_s(target.wantedId, view->monitorId);
    EnumDisplayMonitors(nullptr, nullptr, enum_monitors, reinterpret_cast<LPARAM>(&target));
    RECT bounds{0, 0, static_cast<LONG>(view->width), static_cast<LONG>(view->height)};
    if (target.matches >= 1) {
        bounds = target.info.rcMonitor;
    } else if (target.fallbackNonPrimary) {
        bounds = target.fallbackNonPrimaryInfo.rcMonitor;
    } else if (target.fallbackPrimary) {
        bounds = target.fallbackPrimaryInfo.rcMonitor;
    }
#endif

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ObsNrVfgPreviewWindow";
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        auto* split = reinterpret_cast<SplitState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == workerFinished) return 0;
        if (msg == WM_DISPLAYCHANGE && split) ++split->displayEpoch;
        if (msg == reveal) {
            auto* s = split ? split->session : nullptr;
            if (s && state(*s) == 1) {
                const DWORD affinity = display_affinity(InterlockedCompareExchange(&s->hideFromCapture, 0, 0));
                SetWindowDisplayAffinity(hwnd, affinity);
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
            return 0;
        }
        if (msg == syncAffinity) {
            auto* s = split ? split->session : nullptr;
            const DWORD affinity = s ? display_affinity(InterlockedCompareExchange(&s->hideFromCapture, 0, 0))
                                     : static_cast<DWORD>(wp ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
            SetWindowDisplayAffinity(hwnd, affinity);
            return 0;
        }
        if (split && (msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP ||
            msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE)) {
            std::unique_lock lock(split->mutex);
            if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) {
                split->dragging = false;
                lock.unlock();
                if (msg == WM_CANCELMODE && GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            RECT client{};
            if (!split->enabled || !GetClientRect(hwnd, &client) || client.right <= 0) {
                split->dragging = false;
                return 0;
            }
            const int x = std::clamp(GET_X_LPARAM(lp), 0, static_cast<int>(client.right));
            if (msg == WM_LBUTTONDOWN) {
                const int divider = static_cast<int>(static_cast<int64_t>(client.right) * split->percent / 100);
                if (std::abs(x - divider) > 12) return 0;
                SetCapture(hwnd);
                split->dragging = GetCapture() == hwnd;
            }
            if (split->dragging) {
                split->percent = static_cast<uint32_t>((static_cast<int64_t>(x) * 100 + client.right / 2) / client.right);
                split->overridden = true;
                if (msg == WM_LBUTTONUP) {
                    split->dragging = false;
                    lock.unlock();
                    ReleaseCapture();
                }
            }
            return 0;
        }
        if (msg == WM_CLOSE || (msg == WM_KEYDOWN && wp == VK_ESCAPE)
#ifdef OBS_NR_VFG_QA_ACTION
            || msg == WM_DISPLAYCHANGE
#endif
        ) {
            auto* s = split ? split->session : nullptr;
            if (s) {
                if (msg != WM_DISPLAYCHANGE) {
                    stamp_exit_reason(*s, ExitReason::UserClosed);
                    split->userClosing->store(read_exit_reason(*s) == ExitReason::UserClosed);
                }
                InterlockedExchange64(&s->state, -1);
            }
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    };
    if (!RegisterClassW(&wc)) return 2;

#ifdef OBS_NR_VFG_QA_ACTION
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_NOACTIVATE;
#else
    const DWORD exStyle = WS_EX_NOACTIVATE;
#endif
    auto window = CreateWindowExW(exStyle, wc.lpszClassName, L"OBS NVIDIA VFG",
        WS_POPUP, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return 2;

#ifdef OBS_NR_VFG_QA_ACTION
    RECT actual{};
    if (MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != target.handle ||
        !GetWindowRect(window, &actual) || !EqualRect(&actual, &bounds)) {
        DestroyWindow(window);
        return reject("window_position_mismatch");
    }
    fprintf(file, "event=qa_monitor_selected id=%ls device=%ls rect=%ld,%ld,%ld,%ld primary=0\n",
        target.wantedId, target.info.szDevice, bounds.left, bounds.top, bounds.right, bounds.bottom);
#endif
    SplitState split;
    split.session = view;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&split));
    const LONG initialHide = InterlockedCompareExchange(&view->hideFromCapture, 0, 0) ? 1 : 0;
    const DWORD initialAffinity = display_affinity(initialHide);
    if (!SetWindowDisplayAffinity(window, initialAffinity)) {
        fprintf(file, "event=display_affinity_failed affinity=0x%lX err=%lu\n",
            initialAffinity, GetLastError());
        if (initialHide) { DestroyWindow(window); return 2; }
    } else {
        fprintf(file, "event=display_affinity affinity=0x%lX\n", initialAffinity);
    }

    const auto hudScale = hud::scale_for(view->width, view->height,
        static_cast<uint32_t>(bounds.right - bounds.left), static_cast<uint32_t>(bounds.bottom - bounds.top),
        GetDpiForWindow(window));
    Feed feed{*view, parent.h, window, file, argv[3], cliMult, hudScale, split};
    split.userClosing = &feed.userClosing;
    Handle workerDone;
    workerDone.h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!workerDone.h) {
        fprintf(file, "event=error message=worker_done_event_failed\n");
        DestroyWindow(window);
        return 2;
    }
    const auto watchdog = SetTimer(window, 1, 200, nullptr);
    if (!watchdog) {
        fprintf(file, "event=error message=watchdog_timer_failed\n");
        DestroyWindow(window);
        return 2;
    }
    std::thread worker([&] {
        try {
            while (!feed.stopped()) {
                const auto current = state(*view);
                if (current == 1) {
                    frames(feed);
                } else if (current == 2) {
                    if (InterlockedCompareExchange64(&view->state, 3, 2) == 2)
                        fprintf(file, "event=resize_drained hwnd=%p\n", static_cast<void*>(window));
                } else if (current != 0 && current != 3) {
                    check(E_INVALIDARG, "invalid presenter state");
                }
                feed.beat();
                if (state(*view) != 1) WaitForSingleObject(parent.h, 10);
            }
            feed.result.store(0);
        }
        catch (const std::exception& e) {
            stamp_exit_reason(*view, ExitReason::Fault);
            InterlockedExchange64(&view->state, -1);
            fprintf(file, "event=error message=%s\n", e.what());
            if (const auto stall = fixture_ms(*view, L"OBS_NR_TEST_FAULT_EXIT_DELAY_MS")) Sleep(stall);
            feed.result.store(1);
        }
        SetEvent(workerDone.h);
        PostMessageW(window, workerFinished, 0, 0);
    });
    MSG msg{};
    while (feed.result.load() < 0 && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            const auto initializing = feed.initializing.load();
            const auto heartbeat = feed.heartbeat.load();
            const auto elapsed = clock_ns() - heartbeat;
            const auto timeout = (initializing ? 15000 : 2000) * ms;
            if (feed.stopped()) {
                break;
            } else if (elapsed > timeout) {
                stamp_exit_reason(*view, ExitReason::Watchdog);
                fprintf(file, "event=watchdog_timeout initializing=%d elapsed_ms=%lld\n",
                    initializing ? 1 : 0, static_cast<long long>(elapsed / ms));
                fflush(file);
                break;
            }
            DWORD currentAffinity = WDA_NONE;
            const DWORD wantedAffinity = display_affinity(InterlockedCompareExchange(&view->hideFromCapture, 0, 0));
            if (GetWindowDisplayAffinity(window, &currentAffinity) && currentAffinity != wantedAffinity) {
                if (SetWindowDisplayAffinity(window, wantedAffinity))
                    fprintf(file, "event=display_affinity affinity=0x%lX\n", wantedAffinity);
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (feed.stopped()) break;
    }
    KillTimer(window, watchdog);
    InterlockedExchange64(&view->state, -1);
    ShowWindow(window, SW_HIDE);
    if (WaitForSingleObject(workerDone.h, 1000) != WAIT_OBJECT_0) {
        const auto reason = read_exit_reason(*view);
        fprintf(file, "event=shutdown_force_terminate watchdog=%d exit_reason=%ld\n",
            reason == ExitReason::Watchdog, static_cast<LONG>(reason));
        fflush(file);
        if (reason == ExitReason::UserClosed) clear_pushflow_starting_on_close(feed);
        TerminateProcess(GetCurrentProcess(), reason == ExitReason::UserClosed ? 0u : 2u);
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }
    worker.join();
    DestroyWindow(window);
    const auto reason = read_exit_reason(*view);
    const auto result = feed.result.load();
    fprintf(file, "event=session_finished exit_reason=%ld worker_result=%d\n", static_cast<LONG>(reason), result);
    return reason == ExitReason::Watchdog ? 2 : result == 0 ? 0 : 1;
}
