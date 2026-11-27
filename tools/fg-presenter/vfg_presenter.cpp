// SPDX-License-Identifier: GPL-3.0-only
#include "native_vfg.h"
#include "anr_fg_backend.h"
#include "pushflow_backend.h"
#include "vfg_adaptive_fg.h"
#include "vfg_cadence.h"
#include "vfg_generation_scope.h"
#include "vfg_hud.h"
#include "vfg_live_anchor.h"
#include "vfg_pacing_repair.h"
#include "vfg_reflex.h"
#include "vfg_trace.h"
#include <mmsystem.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <deque>
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
    std::atomic_bool pushflowActive{false};
    std::atomic<LONG> pendingAffinityLog{-1};
    pushflow::PreviewMultiplier previewMultiplier;
    TraceSink trace;
    bool stopped() { return state(view) < 0 || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT; }
    bool active() { return state(view) == 1 && !stopped(); }
    void beat() { heartbeat.store(clock_ns()); }
};

struct SplitState {
    std::mutex mutex;
    Session* session{};
    std::atomic_uint64_t displayEpoch{};
    bool enabled{}, dragging{}, initialized{}, overridden{}, swap{};
    uint32_t observed{50}, percent{50};

    void reset(bool on) {
        std::lock_guard lock(mutex);
        enabled = on;
        dragging = initialized = overridden = swap = false;
        observed = percent = 50;
    }
    uint32_t update(uint32_t value, bool swapSides) {
        std::lock_guard lock(mutex);
        value = std::min(value, 100u);
        if (!initialized || value != observed) {
            observed = value;
            initialized = true;
            overridden = false;
        }
        if (!overridden) percent = observed;
        swap = swapSides;
        return percent;
    }
    bool swapped() {
        std::lock_guard lock(mutex);
        return swap;
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

struct LogWriteScope {
    Session& view;
    explicit LogWriteScope(Session& v) noexcept : view(v) { InterlockedIncrement(&view.logWriteInFlight); }
    ~LogWriteScope() { InterlockedDecrement(&view.logWriteInFlight); }
};

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
    unsigned shotSlot{};
};

template<class... Fields>
void write_trace(Feed& f, bool force, const char* format, Fields... fields)
{
    if (!force && !f.trace.enabled) return;
    char line[640];
    const int len = snprintf(line, sizeof(line), format, fields...);
    if (len > 0) {
        const int bytes = std::min<int>(len, static_cast<int>(sizeof(line) - 1));
        const LogWriteScope ioScope{f.view};
        (void)f.trace.send(line, bytes, force);
    }
}

template<class... Fields>
void trace(Feed& f, const char* format, Fields... fields)
{
    write_trace(f, false, format, fields...);
}

template<class... Fields>
void trace_event(Feed& f, const char* format, Fields... fields)
{
    write_trace(f, true, format, fields...);
}

void record(Feed& f, const char* kind, const ipc::Frame& b, const ipc::Frame* a, UINT id,
    const PairTelemetry& telemetry, uint64_t workNs = 0, uint64_t eventNs = 0)
{
    const auto q = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    trace(f,
        "event=%s generation=%llu b=%llu b_qpc_ns=%llu b_arrival_ns=%llu b_obs_tick=%llu a=%llu a_qpc_ns=%llu present_id=%u qpc_ns=%llu work_ns=%llu eff_mult=%u pacing_shift_ns=%lld hud_lock=%d shot_cut=%d pacing_repair=%d adaptive_fg=%d adaptive_target_fps=%ld shot_slot=%u\n",
        kind, q(b.generation), q(b.serial), q(b.timestampNs), q(b.arrivalNs), q(b.obsTick), q(a ? a->serial : 0),
        q(a ? a->timestampNs : 0), id, q(eventNs ? eventNs : clock_ns()), q(workNs), telemetry.multiplier,
        static_cast<long long>(telemetry.shiftNs), telemetry.hudLock, telemetry.shotCut,
        telemetry.pacingRepair, telemetry.adaptiveFg, telemetry.adaptiveTargetFps, telemetry.shotSlot);
}

struct PendingPair {
    ContentFrame prevFrame, currFrame;
    PairTelemetry telemetry;
    CadencePlan plan;
    uint64_t deadlineNs{}, fid{}, hudEpoch{}, extraNs{};
    std::optional<FgPairTicket> ticket{};
    unsigned nextSub{1}, shotParity{};
    bool repeat{}, updateHud{true}, resetHud{};
    // Split repeats own a presentation slot, never the unique-content anchor.
    const ContentFrame& anchor() const { return repeat ? prevFrame : currFrame; }
    uint64_t due() const {
        return ticket && nextSub <= ticket->lastSubmittedSub ? plan.target(nextSub) : plan.originalNs;
    }
};

struct HalfRate {
    std::optional<uint64_t> lastNew, first;
    bool skip(DedupMode mode, uint64_t serial, bool same) {
        if (mode != DedupMode::Adaptive60To30 && mode != DedupMode::Fixed60To30) {
            first.reset();
            if (!same) lastNew = serial;
            return same;
        }
        if (!first && same && lastNew && serial - *lastNew == 1) first = lastNew;
        const bool second = first && (serial - *first) % 2 == 1;
        if (mode == DedupMode::Adaptive60To30 && second && !same) first.reset();
        if (mode == DedupMode::Fixed60To30 && first) return second;
        if (!same) lastNew = serial;
        return same;
    }
};

// The HWND runs on another thread. IPC and GPU signals interrupt the nearest
// presentation deadline; the timed poll also covers missing/coalesced signals.
void scheduler_wait(uint64_t targetNs, HANDLE timer, const std::array<HANDLE, 3>& sources)
{
    const auto now = clock_ns();
    if (now >= targetNs) return;
    std::array<HANDLE, 4> handles{};
    DWORD count = 0;
    for (const auto h : sources) if (h) handles[count++] = h;
    const auto remaining = targetNs - now;
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(std::max<uint64_t>(1, remaining / 100));
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) handles[count++] = timer;
    if (MsgWaitForMultipleObjects(count, handles.data(), FALSE,
            static_cast<DWORD>((remaining + ms - 1) / ms), QS_ALLINPUT) == WAIT_FAILED)
        check(HRESULT_FROM_WIN32(GetLastError()), "scheduler wait");
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
        InterlockedExchange(&f.view.drainFailed, 1);
        const bool userClosed = f.view.backend == Backend::PushFlow &&
            read_exit_reason(f.view) == ExitReason::UserClosed;
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
            InterlockedExchange(&f.view.pushflowActive, 1);
            if (pushflowBackend->open({f.view, f.log, [&f] { f.beat(); }, userClosing, 0, &f.previewMultiplier}, &gpu))
                vfg = pushflowBackend;
            else if (pushflowBackend->runtime_fault())
                check(E_FAIL, "PushFlow initialization fault");
            else {
                f.pushflowActive.store(false);
                InterlockedExchange(&f.view.pushflowActive, 0);
            }
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
        case Backend::XeFg:
            // Todo 6 installs the official XeFG presenter; until then a XeFG
            // session must degrade to the original frame instead of a null backend.
            fprintf(f.log, "event=xefg_backend_unavailable_fallback_1x\n");
            break;
        case Backend::XeMfg:
            // Todo 7 installs the opt-in XeMFG backend; same safe 1x contract.
            fprintf(f.log, "event=xemfg_backend_unavailable_fallback_1x\n");
            break;
        }
    }
    // A 4x pair's three subframes share one 60 Hz period, leaving ~4.2 ms per
    // slot. A synchronous pair enqueue measures 4.3..6.1 ms, so admitting a
    // second pair while the current one presents pushes that pair's subframes
    // past their cutoffs: +0.6..+2.3 ms original latency and late drops against
    // the single-pair baseline. 2x's ~8.3 ms windows absorb the overlap, so
    // only the 4x path stays single-pair.
    const size_t backendCapacity = vfg ? vfg->max_in_flight_pairs() : 1;
    const size_t batchCapacity = mult == max_multiplier ? 1 : backendCapacity;
    fprintf(f.log, "event=initialized backend=%s sdk_version=0x%08X low_latency=%ld gpu_shot_change=%u hud_lock=%ld pacing_repair=%ld adaptive_fg=%ld batch_capacity=%zu\n",
        vfg ? vfg->name() : "original", vfg ? vfg->version_tag() : 0u, lastReflex, f.view.gpuShotChange,
        InterlockedCompareExchange(&f.view.hudLock, 0, 0), InterlockedCompareExchange(&f.view.pacingRepair, 0, 0),
        InterlockedCompareExchange(&f.view.adaptiveFg, 0, 0), batchCapacity);
    f.beat();
    f.initializing.store(false);
    if (const auto stall = fixture_ms(f.view, L"OBS_NR_TEST_STALL_WORKER_MS")) {
        fprintf(f.log, "event=test_worker_stalled delay_ms=%lu\n", stall);
        Sleep(stall);
    }
    if (const auto lockStall = fixture_ms(f.view, L"OBS_NR_TEST_STALL_LOG_LOCK_MS")) {
        _lock_file(f.log);
        fprintf(f.log, "event=test_log_locked delay_ms=%lu\n", lockStall);
        Sleep(lockStall);
        _unlock_file(f.log);
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
    uint64_t lastPeriod{}, lastLockedPeriod{};
    bool sawRepeat = false;
    PushPacingRepair repair;
    TransportIntervalEstimator transport;
    ContentRateTracker rate;
    AdaptiveMultiplierGate multiplierGate;
    uint64_t previousRepairedPtsNs{}, uniqueOrdinal{}, lastChangedNs{}, previousObsTick{};
    int64_t transportNs = TransportIntervalEstimator::kDefaultNs;
    bool staticHold = false, wasRepairing = false, wasAdaptive = false, backendFault = false;
    bool hudEnabled = false, hudReset = true;
    uint64_t hudEpoch = 0;
    const auto sync_hud_lock = [&] {
        const bool enabled = InterlockedCompareExchange(&f.view.hudLock, 0, 0) != 0;
        if (enabled != hudEnabled) { hudReset = true; ++hudEpoch; }
        hudEnabled = enabled;
        gpu.sync_hud_lock(enabled);
    };
    double contentFps = 0;
    auto displayEpoch = f.split.displayEpoch.load();
    auto monitorHz = window_refresh(f.window);
    std::deque<PendingPair> queue;
    ComPtr<ID3D12Fence> pairRetireFence;
    if (vfg) check(gpu.d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pairRetireFence)),
        "pair retirement fence");
    struct Retired { FgPairTicket ticket; uint64_t value, since; };
    std::array<std::optional<Retired>, batch_capacity> retired{};
    std::array<uint64_t, batch_capacity> lastRetire{};
    uint64_t retireSeq = 0, recoverySince = 0;
    LiveAnchorClock liveClock;
    HalfRate halfRate;
    DedupMode lastMode = DedupMode::OffExact;
    bool wasLive = false;
    uint64_t dedupSamples = 0, repeats = 0, decimated = 0;
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
        ipc::CopyTicket copy;
        PairTelemetry telemetry;
        uint64_t receivedNs;
        unsigned index;
        bool continuous, interpolate;
        DedupMode mode;
        bool live;
        uint64_t compareStart{};
    };
    std::optional<Incoming> incoming;
    uint64_t reflexFrameId = 0;
    const auto retire = [&](const char* reason) {
        const auto& p = queue.front();
        if (p.ticket) {
            const auto& ticket = *p.ticket;
            check(gpu.drawQueue->Signal(pairRetireFence.Get(), ++retireSeq), "pair retirement signal");
            vfg->retire_pair(ticket, pairRetireFence.Get(), retireSeq);
            lastRetire[ticket.slotParity] = retireSeq;
            retired[ticket.slotParity].emplace(Retired{ticket, retireSeq, clock_ns()});
            trace(f, "event=pair_retired b=%llu bank=%u fence=%llu reason=%s depth=%zu\n",
                p.currFrame.frame.serial, ticket.slotParity, retireSeq, reason, queue.size());
        }
    };
    const auto drain = [&](const char* reason) {
        while (!queue.empty()) { retire(reason); queue.pop_front(); }
    };
    const auto reset = [&](const char* reason) {
        drain(reason);
        history.invalidate();
        gpu.discard_compare();
        if (incoming) { incoming->continuous = false; incoming->compareStart = 0; }
        hudReset = true;
        cadence.reset(); repair.reset(); rate.reset(); liveClock.reset(); halfRate = {};
        transport.reset(); previousObsTick = 0;
        lastOriginalTarget = lastPeriod = lastLockedPeriod = previousRepairedPtsNs = lastChangedNs = 0;
        sawRepeat = staticHold = false;
    };
    const auto ingest = [&] {
        std::array<unsigned, 2 * batch_capacity> pins{};
        size_t count = 0;
        for (const auto& p : queue) { pins[count++] = p.prevFrame.index; pins[count++] = p.currFrame.index; }
        const auto index = history.input_index(std::span(pins).first(count));
        if (index == content_input_count) return;
        gpu.prepare_input(index);
        const auto next = consumer.copy_next(gpu.input[index].Get(), split ? gpu.before[index].Get() : nullptr);
        if (!next.current) {
            if (next.status != ipc::Status::Pending && next.status != ipc::Status::Dropped) check(E_FAIL, "IPC copy_next");
            return;
        }
        if (next.status != ipc::Status::Ok || consumer.order_copy(*next.current, gpu.drawQueue.Get()) != ipc::Status::Ok)
            check(E_FAIL, "IPC copy dependency");
        const auto& b = next.current->frame;
        const auto receivedNs = clock_ns();
        if (split) f.split.update(b.splitPercent, b.splitSwap);
        if (vfg && vfg->faulted() && !backendFault) {
            backendFault = true;
            fprintf(f.log, "event=backend_fault_fallback_1x backend=%s\n", vfg->name());
        }
        const PairTelemetry telemetry{1, 0, hudEnabled, false,
            InterlockedCompareExchange(&f.view.pacingRepair, 0, 0) != 0, InterlockedCompareExchange(&f.view.adaptiveFg, 0, 0) != 0,
            InterlockedCompareExchange(&f.view.adaptiveTargetFps, 0, 0)};
        const auto step = history.observe(index, b, next.previous.has_value());
        if (step.stale) {
            record(f, "input", b, nullptr, 0, telemetry, 0, receivedNs);
            record(f, "drop_original_stale", b, nullptr, 0, telemetry);
            return;
        }
        if (!step.continuous) reset("discontinuity");
        const auto tick = b.obsTick ? b.obsTick : b.timestampNs;
        if (tick <= INT64_MAX) transportNs = transport.observe(static_cast<int64_t>(tick));
        const auto mode = static_cast<DedupMode>(InterlockedCompareExchange(&f.view.dedupMode, 0, 0));
        const bool live = InterlockedCompareExchange(&f.view.liveAnchor, 0, 0) != 0;
        if (lastMode != mode) { halfRate = {}; rate.reset(); lastMode = mode; }
        if (wasLive != live) { liveClock.reset(); lastOriginalTarget = 0; wasLive = live; }
        incoming.emplace(Incoming{*next.current, telemetry, receivedNs, index, step.continuous, vfg && !backendFault, mode, live});
    };
    const auto advance = [&](uint64_t& wake) {
        auto& p = queue.front();
        const auto now = clock_ns();
        const auto& b = p.currFrame.frame;
        const auto& a = p.prevFrame.frame;
        const auto index = p.currFrame.index;
        if (now > p.deadlineNs) {
            record(f, "drop_original_late", b, nullptr, 0, p.telemetry);
            reset("late");
            return true;
        }
        const auto due = p.due();
        if (now < due) { wake = std::min(wake, due); return false; }
        const auto retry = [&] { wake = std::min(wake, clock_ns() + 350'000); return false; };
        const bool compose = p.telemetry.hudLock && hudEnabled && p.hudEpoch == hudEpoch;
        if (compose && p.updateHud && !p.repeat) {
            if (!gpu.commands_ready()) return retry();
            gpu.update_hud_lock(index, p.prevFrame.index, p.resetHud);
            p.updateHud = false;
        }
        if (p.ticket && p.nextSub <= p.ticket->lastSubmittedSub) {
            const auto& ticket = *p.ticket;
            const unsigned sub = p.nextSub;
            const auto cutoff = std::min(p.plan.target(sub + 1), p.plan.synthetic_cutoff(sub) + p.extraNs);
            bool ready = false, suppressed = false;
            if (now < cutoff) {
                if (anrBackend) {
                    switch (anrBackend->output(ticket, sub)) {
                    case AnrFgBackend::Output::Ready: ready = true; break;
                    case AnrFgBackend::Output::Suppressed: suppressed = true; break;
                    case AnrFgBackend::Output::Pending: break;
                    }
                } else ready = vfg->subframe_ready(ticket, sub);
                if (!ready && !suppressed) { wake = std::min(wake, cutoff); return retry(); }
            }
            if (ready) {
                refresh_hud(false);
                reflex.oob_marker(ReflexMarker::OutOfBandRenderSubmitStart, p.fid, sub);
                reflex.oob_marker(ReflexMarker::OutOfBandRenderSubmitEnd, p.fid, sub);
                reflex.oob_marker(ReflexMarker::OutOfBandPresentStart, p.fid, sub);
                auto* texture = gpu.syntheticTex[ticket.slotParity * (max_multiplier - 1) + sub - 1].Get();
                const Gpu::SyntheticFrame synthetic{texture, [&](ID3D12GraphicsCommandList* cmd) {
                    vfg->resolve_subframe(cmd, ticket, texture, sub);
                }, ticket.prevIndex, ticket.currIndex, f.view.gpuShotChange != 0, p.shotParity, compose, sub - 1, ticket.slotParity};
                const auto id = gpu.try_present_synthetic(synthetic, cutoff, split ? static_cast<int>(index) : -1,
                    split ? f.split.current() : 50, split && f.split.swapped());
                reflex.oob_marker(ReflexMarker::OutOfBandPresentEnd, p.fid, sub);
                record(f, id ? "present_vfg_subframe" : "drop_vfg_present_busy", b, &a, id.value_or(0), p.telemetry, now - due);
            } else record(f, suppressed ? "drop_vfg_provider_disabled" : "drop_vfg_subframe_late", b, &a, 0, p.telemetry);
            ++p.nextSub;
            return true;
        }
        if (!gpu.commands_ready()) return retry();
        if (!shown) { PostMessageW(f.window, reveal, 0, 0); shown = true; }
        refresh_hud(true);
        if (!p.repeat) reflex.marker(ReflexMarker::PresentStart, p.fid);
        const UINT pid = gpu.present(gpu.input[index].Get(), false, split ? static_cast<int>(index) : -1,
            split ? f.split.current() : 50, split && f.split.swapped());
        if (!p.repeat) reflex.marker(ReflexMarker::PresentEnd, p.fid);
        record(f, p.repeat ? split ? "present_repeat_split" : "present_repeat_original" : "present_original", b, nullptr, pid, p.telemetry);
        if (!p.repeat) { history.retain(index, b); gpu.enter_steady_state(); }
        retire("present");
        if (!p.repeat) lastOriginalTarget = p.plan.originalNs;
        queue.pop_front();
        if (faultExitDelay && !faultArm.h) throw std::runtime_error("test_live_consumer_fault");
        return true;
    };
    const auto log_dedup = [&] {
        trace(f, "[content-dedup] mode=%u samples=%llu repeat=%llu decimated=%llu content_fps=%.3f half_locked=%d\n",
            static_cast<unsigned>(lastMode), dedupSamples, repeats, decimated, contentFps, int(halfRate.first.has_value()));
    };
    const auto admit = [&] {
        const auto b = incoming->copy.frame;
        const auto index = incoming->index;
        const auto continuous = incoming->continuous;
        const auto anchor = queue.empty() ? history.anchor() : std::optional{queue.back().anchor()};
        auto telemetry = incoming->telemetry;
        telemetry.hudLock &= hudEnabled;
        const bool adaptive = telemetry.adaptiveFg, repairing = telemetry.pacingRepair;
        const bool interpolate = incoming->interpolate, live = incoming->live;
        const auto mode = incoming->mode;
        uint64_t comparedNs{}, compareWorkNs{};
        uint32_t shotParity = 0;
        const auto record_input = [&](const char* comparison = "content_changed") {
            record(f, "input", b, nullptr, 0, telemetry, 0, incoming->receivedNs);
            if (comparedNs) record(f, comparison, b, &anchor->frame, 0, telemetry, compareWorkNs, comparedNs);
            incoming.reset();
        };
        if (clock_ns() > b.arrivalNs + 50 * ms) {
            record_input();
            record(f, "drop_original_late", b, nullptr, 0, telemetry);
            reset("late");
            return true;
        }
        // Avoid ever entering a blocking Present behind an uncompleted IPC dependency.
        const auto status = consumer.copy_status(incoming->copy);
        if (status == ipc::Status::Pending) return false;
        if (status != ipc::Status::Ok) check(E_FAIL, "IPC copy completion");
        bool same = false;
        if ((interpolate || telemetry.hudLock) && anchor) {
            if (!incoming->compareStart) {
                while (std::any_of(queue.begin(), queue.end(), [&](const auto& p) {
                    return p.ticket && p.shotParity == shotParity;
                })) ++shotParity;
                if (!gpu.begin_compare(anchor->index, index, compareReady.h, static_cast<uint32_t>(mode), shotParity)) return false;
                incoming->compareStart = clock_ns();
            }
            const auto compared = gpu.compare_result();
            if (!compared) return false;
            comparedNs = clock_ns(); compareWorkNs = comparedNs - incoming->compareStart;
            telemetry.shotSlot = shotParity = compared->parity; telemetry.shotCut = compared->cut; same = compared->same;
        } else if (incoming->compareStart) gpu.discard_compare();
        const auto tick = b.obsTick ? b.obsTick : b.timestampNs;
        const auto interval = f.view.transportIntervalNs ? f.view.transportIntervalNs :
            previousObsTick && tick > previousObsTick ? tick - previousObsTick : 0;
        previousObsTick = tick;
        const bool transport60 = interval >= 16'393'443 && interval <= 16'949'152;
        const bool skip = halfRate.skip(transport60 ? mode : DedupMode::OffExact, b.serial, same);
        ++dedupSamples;
        if (skip && same) ++repeats;
        if (skip && !same) ++decimated;
        if (skip && anchor) {
            if (lastChangedNs && b.timestampNs >= lastChangedNs && b.timestampNs - lastChangedNs >= 500 * ms) staticHold = true;
            contentFps = rate.observe(b.timestampNs, uniqueOrdinal, staticHold);
            sawRepeat = true;
            record_input(same ? "repeat_input" : "decimate_input");
            if (!interpolate || split) {
                auto& p = queue.emplace_back(PendingPair{*anchor, {b, index}, telemetry, {}, b.arrivalNs + 50 * ms});
                p.repeat = true;
            }
            if (dedupSamples % 30 == 0) log_dedup();
            return true;
        }
        staticHold = false;
        lastChangedNs = b.timestampNs;
        if (repairing != wasRepairing) {
            cadence.reset();
            repair.reset();
            previousRepairedPtsNs = lastOriginalTarget = lastPeriod = lastLockedPeriod = 0;
            wasRepairing = repairing;
        }
        const bool useObsTick = mode == DedupMode::OffExact && b.obsTick != 0;
        auto timestamp = useObsTick ? b.obsTick : b.timestampNs;
        auto phaseArrivalNs = b.arrivalNs;
        if (repairing) {
            if (timestamp > INT64_MAX || phaseArrivalNs > INT64_MAX) { record_input(); return true; }
            const auto raw = static_cast<int64_t>(timestamp);
            const auto result = repair.apply(raw / 100, raw / 100, transportNs / 100, !continuous);
            if (result.disposition == PushPacingRepair::Disposition::noRepresentableProgress) { record_input(); return true; }
            const auto repaired = repaired_times(raw, static_cast<int64_t>(phaseArrivalNs), result);
            if (!repaired || repaired->repairedContentPtsNs < 0 || repaired->phaseArrivalNs < 0) { record_input(); return true; }
            timestamp = static_cast<uint64_t>(repaired->repairedContentPtsNs);
            telemetry.shiftNs = result.shift * 100;
            phaseArrivalNs = static_cast<uint64_t>(repaired->phaseArrivalNs);
        }
        telemetry.multiplier = interpolate ? mult : 1u;
        contentFps = rate.observe(b.timestampNs, ++uniqueOrdinal, staticHold);
        // A live-anchor session accepts the 24 Hz band's alternating 33/50 ms
        // transport ticks; liveAnchor=0 keeps the pre-v5 gate so a 17/50 ms
        // dedup burst is a discontinuity, not a 30 Hz cadence lock.
        const auto lockedPeriod = interpolate ? cadence.observe(timestamp,
            live ? cadence_max_interval_live_ns : cadence_max_interval_legacy_ns) : 0;
        if (lockedPeriod && lockedPeriod != lastLockedPeriod) {
            trace_event(f, "event=cadence_locked period_ns=%llu repairing=%d\n",
                static_cast<unsigned long long>(lockedPeriod), repairing ? 1 : 0);
        }
        lastLockedPeriod = lockedPeriod;
        auto period = lockedPeriod;
        const auto previous = repairing ? previousRepairedPtsNs :
            anchor ? (useObsTick ? anchor->frame.obsTick : anchor->frame.timestampNs) : 0;
        if (!period && interpolate && anchor && (repairing || mode != DedupMode::OffExact ||
                (b.obsTick != 0) == (anchor->frame.obsTick != 0)) && previous) {
            if (provisional_120hz(previous, timestamp, sawRepeat)) period = period_120hz;
            else if (provisional_60hz(previous, timestamp, sawRepeat)) period = period_60hz;
        }
        bool variable = false;
        if (interpolate && halfRate.first) period = period_30hz;
        else if (!period && interpolate && mode != DedupMode::OffExact && previous && timestamp > previous) {
            period = timestamp - previous;
            variable = true;
        }
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
            if (contentFps > 0 && !halfRate.first)
                period = static_cast<uint64_t>(std::clamp(1e9 / contentFps, 7'500'000.0, 50'000'000.0) + 0.5);
        }
        const auto activeMult = telemetry.multiplier;
        // Consult the last *scheduled* target, not just the last original already displayed.
        const auto predecessor = queue.empty() ? lastOriginalTarget : queue.back().plan.originalNs;
        const auto previousTarget = !live && (adaptive || period == lastPeriod) ? predecessor : 0;
        lastPeriod = period;
        auto plan = !anchor || !period ? CadencePlan{} : adaptive || variable ?
            plan_cadence_adaptive(phaseArrivalNs, previousTarget, period, activeMult) :
            plan_cadence(phaseArrivalNs, previousTarget, period, activeMult);
        LiveAnchorClock::Timing timing{};
        if (live) {
            const auto phase = plan.originalNs ? plan.originalNs - phaseArrivalNs : 0;
            timing = liveClock.observe({b.timestampNs, b.arrivalNs}, phase);
            // Pacing repair owns the content clock. Re-applying the raw callback
            // phase on top would reintroduce the source cadence the repair
            // removes, so the repaired cadence plan keeps the target. The clock
            // still observes every original, and a held frame probes admission.
            if (plan.originalNs && !repairing)
                plan.originalNs = std::max(b.arrivalNs, timing.anchorNs + phase);
        }
        // A bunched callback can reseed the cadence estimator before the front
        // original. Never schedule its successor's first slot in that old pair.
        if (!queue.empty() && plan.originalNs && plan.target(1) <= predecessor)
            plan.originalNs = predecessor + plan.periodNs;
        record_input();
        if (dedupSamples % 30 == 0) log_dedup();
        const auto fid = ++reflexFrameId;
        const ContentFrame current{b, index};
        auto& pending = queue.emplace_back(PendingPair{anchor.value_or(current), current, telemetry, plan, 0, fid, hudEpoch});
        pending.shotParity = shotParity;
        pending.resetHud = hudReset || telemetry.shotCut || !anchor;
        if (telemetry.hudLock) hudReset = false;
        reflex.sleep(clock_ns(), clock_ns);
        reflex.marker(ReflexMarker::SimulationStart, fid);
        reflex.marker(ReflexMarker::SimulationEnd, fid);
        reflex.marker(ReflexMarker::RenderSubmitStart, fid);
        if (interpolate && anchor && plan.originalNs) {
            const auto& a = anchor->frame;
            const auto stageStart = clock_ns();
            const bool pairLate = stageStart >= plan.synthetic_cutoff(activeMult - 1);
            const FgPairDesc pair{gpu.input[anchor->index].Get(), gpu.input[index].Get(), a.serial, b.serial, activeMult,
                !continuous, telemetry.shotCut,
                [&] { record(f, "stage_queued", b, &a, 0, telemetry, clock_ns() - stageStart); },
                [&](uint64_t workNs) { record(f, pushflowBackend ? "pushflow_enqueued" : "cuda_enqueued", b, &a, 0, telemetry, workNs); }};
            if (const auto ticket = pairLate ? std::nullopt : vfg->try_submit_pair(pair, anchor->index, index)) {
                pending.ticket.emplace(*ticket);
                retired[ticket->slotParity].reset(); // The backend proved both GPU timelines complete.
                trace(f, "event=pair_admitted b=%llu bank=%u shot_slot=%u depth=%zu prior_fence=%llu completed=%llu\n",
                    b.serial, ticket->slotParity, shotParity, queue.size(), lastRetire[ticket->slotParity], pairRetireFence->GetCompletedValue());
            } else {
                if (anrBackend && anrBackend->faulted()) pending.telemetry.multiplier = 1;
                const bool waiting = std::any_of(retired.begin(), retired.end(), [](const auto& bank) { return bank.has_value(); });
                record(f, pairLate ? "drop_vfg_pair_late" : waiting ? "drop_vfg_gpu_busy" : "drop_vfg_stage_busy", b, &a, 0, pending.telemetry);
            }
        } else if (vfg && !continuous) record(f, "drop_vfg_discontinuity", b, nullptr, 0, telemetry);
        reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
        if (live) {
            timing = liveClock.resolve_fg(pending.ticket.has_value());
            if (plan.originalNs) pending.plan.originalNs = timing.fgHeld ? timing.realTargetNs : plan.originalNs;
            pending.extraNs = timing.deadline_extra_ns();
            if (uniqueOrdinal % 15 == 0) trace(f,
                "[live-anchor] b=%llu locked=%d clockDriftPpm=%.2f callbackMinusAnchorMs=%.3f callbackJitterMs=%.3f phaseMs=%.3f deadlineExtraMs=%.3f snaps=%llu fgHeld=%d real_target_ns=%llu\n",
                b.serial, timing.locked, timing.clockDriftPpm, timing.callbackMinusAnchorMs, timing.callbackJitterMs,
                timing.phaseMs, timing.deadlineExtraMs, timing.snaps, timing.fgHeld, pending.plan.originalNs ? pending.plan.originalNs : b.arrivalNs);
        }
        pending.deadlineNs = pending.plan.originalNs ? pending.plan.originalNs + 5 * ms + pending.extraNs : b.arrivalNs + 50 * ms;
        return true;
    };
    while (f.active() && !(pushflowBackend && pushflowBackend->runtime_fault())) {
        if (faultArm.h && WaitForSingleObject(faultArm.h, 0) == WAIT_OBJECT_0) throw std::runtime_error("test_live_consumer_fault");
        if (const LONG aff = f.pendingAffinityLog.exchange(-1, std::memory_order_acq_rel); aff >= 0)
            fprintf(f.log, "event=display_affinity affinity=0x%lX\n", static_cast<DWORD>(aff));
        sync_reflex(); sync_affinity(); sync_hud_lock();
        const auto now = clock_ns();
        auto wake = now + 4 * ms;
        bool stalled = false;
        for (auto& bank : retired) if (bank) {
            const auto completed = pairRetireFence->GetCompletedValue();
            if (completed == UINT64_MAX) check(DXGI_ERROR_DEVICE_REMOVED, "pair retirement device lost");
            if (completed >= bank->value && (backendFault || (anrBackend ? anrBackend->pair_complete(bank->ticket) :
                    vfg->subframe_ready(bank->ticket, bank->ticket.lastSubmittedSub)))) {
                bank.reset();
            } else if (now - bank->since >= 1500 * ms) {
                if (!recoverySince) {
                    trace_event(f, "event=error reason=bank_wait_timeout bank=%u fence=%llu completed=%llu waited_ms=%llu\n",
                        bank->ticket.slotParity, bank->value, completed, (now - bank->since) / ms);
                    recoverySince = now;
                    reset("bank_timeout");
                }
                stalled = true;
                break;
            }
        }
        if (stalled && now - recoverySince >= 240 * ms) drain_failure();
        if (!stalled) {
            if (recoverySince) { trace_event(f, "event=bank_wait_recovered\n"); recoverySince = 0; }
            // One private IPC lookahead can detect a boundary while both pairs
            // are live. It does not compare or enter the backend until Step B.
            if (!incoming) ingest();
            if (!f.active()) break;
            const bool advanced = !queue.empty() && advance(wake);
            const bool admitted = incoming && queue.size() < batchCapacity && admit();
            if (advanced || admitted) { f.beat(); continue; }
            if (incoming && queue.size() < batchCapacity) wake = std::min(wake, clock_ns() + 350'000);
        }
        f.beat();
        scheduler_wait(wake, paceTimer.h, {f.parent, consumer.frame_event(), compareReady.h});
    }
    drain(state(f.view) == 2 ? "resize" : "stop");
    if (incoming) record(f, "input", incoming->copy.frame, nullptr, 0, incoming->telemetry, 0, incoming->receivedNs);
    if (dedupSamples) log_dedup();
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
    f.pushflowActive.store(false);
    InterlockedExchange(&f.view.pushflowActive, 0);
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

    const bool isPipeLog = wcsncmp(argv[4], L"\\\\.\\pipe\\", 9) == 0;
    const HANDLE logHandle = CreateFileW(argv[4],
        isPipeLog ? GENERIC_WRITE : FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        isPipeLog ? OPEN_EXISTING : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
                if (msg != WM_DISPLAYCHANGE)
                    stamp_exit_reason(*s, ExitReason::UserClosed);
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
    const auto traceConfig = trace_config_from_env();
    Feed feed{*view, parent.h, window, file, argv[3], cliMult, hudScale, split};
    feed.trace.file = logHandle;
    feed.trace.stream = file;
    feed.trace.enabled = traceConfig.enabled;
    feed.trace.limitBytes = traceConfig.limitBytes;
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
                InterlockedExchange(&view->watchdogInitializing, initializing ? 1 : 0);
                InterlockedExchange64(&view->watchdogElapsedMs,
                    static_cast<LONG64>(std::max<int64_t>(1, elapsed / ms)));
                stamp_exit_reason(*view, ExitReason::Watchdog);
                break;
            }
            DWORD currentAffinity = WDA_NONE;
            const DWORD wantedAffinity = display_affinity(InterlockedCompareExchange(&view->hideFromCapture, 0, 0));
            if (GetWindowDisplayAffinity(window, &currentAffinity) && currentAffinity != wantedAffinity) {
                if (SetWindowDisplayAffinity(window, wantedAffinity))
                    feed.pendingAffinityLog.store(static_cast<LONG>(wantedAffinity), std::memory_order_release);
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
        InterlockedExchange(&view->forceTerminated, 1);
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
