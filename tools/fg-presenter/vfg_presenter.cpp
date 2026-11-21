// SPDX-License-Identifier: GPL-3.0-only
#include "native_vfg.h"
#include "vfg_cadence.h"
#include "vfg_hud.h"
#include "vfg_reflex.h"
#include <mmsystem.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <intrin.h>
#include <mutex>
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
    bool stopped() { return state(view) < 0 || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT; }
    bool active() { return state(view) == 1 && !stopped(); }
    void beat() { heartbeat.store(clock_ns()); }
};

struct SplitState {
    std::mutex mutex;
    Session* session{};
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

void record(Feed& f, const char* kind, const ipc::Frame& b, const ipc::Frame* a, UINT id, uint64_t workNs = 0)
{
    const auto q = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    fprintf(f.log, "event=%s generation=%llu b=%llu b_qpc_ns=%llu b_arrival_ns=%llu b_obs_tick=%llu a=%llu a_qpc_ns=%llu present_id=%u qpc_ns=%llu work_ns=%llu\n",
        kind, q(b.generation), q(b.serial), q(b.timestampNs), q(b.arrivalNs), q(b.obsTick), q(a ? a->serial : 0),
        q(a ? a->timestampNs : 0), id, q(clock_ns()), q(workNs));
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
    TimerPeriodGuard timerRes;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (!f.active()) return;
    f.beat();
    Handle compareReady, paceTimer;
    compareReady.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!compareReady.h) check(HRESULT_FROM_WIN32(GetLastError()), "comparison event");
    paceTimer.h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (f.view.width < 320 || f.view.height < 180 || f.view.width > 7680 || f.view.height > 4320 ||
        !f.view.generation || !f.view.mapping[0])
        check(E_INVALIDARG, "unsupported session offer");
    const uint32_t mult = f.cliMultiplier <= 1 ? 1 : std::clamp<uint32_t>(f.view.multiplier ? f.view.multiplier : f.cliMultiplier, 2, max_multiplier);
    fprintf(f.log, "event=offer mode=%ux size=%ux%u quality=%u generation=%llu luid=%08lx:%08lx hwnd=%p pid=%lu\n",
        mult, f.view.width, f.view.height, f.view.quality,
        static_cast<unsigned long long>(f.view.generation), static_cast<unsigned long>(f.view.adapter.HighPart),
        f.view.adapter.LowPart, static_cast<void*>(f.window), GetCurrentProcessId());
    set_gpu_process_priority(InterlockedCompareExchange(&f.view.gpuPriority, 0, 0) != 0, f.log);
    const bool split = f.view.splitCompare != 0;
    f.split.reset(split);
    Gpu gpu(f.view, f.window, mult);
    ReflexContext reflex(gpu.d3d_device(), gpu.drawQueue.Get(), f.log);
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
    ipc::Consumer consumer(gpu.ipcQueue.Get(), {f.view.mapping, f.view.generation}, ipc::identity(f.parent));
    std::unique_ptr<NativeVfg> vfg;
    if (mult > 1) vfg = std::make_unique<NativeVfg>(f.runtime, gpu, f.view);
    fprintf(f.log, "event=initialized backend=%s sdk_version=0x%08X low_latency=%ld\n",
        vfg ? "nvidia-vfg-native" : "original", vfg ? vfg->sdk_version() : 0u, lastReflex);
    f.beat();
    bool shown = false;
    ContentHistory history;
    UniqueCadence cadence;
    uint64_t lastOriginalTarget{};
    uint64_t lastPeriod{};
    bool sawRepeat = false;
    bool cudaPending = false;
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
        unsigned index;
        bool continuous;
        uint64_t compareStart{};
    };
    std::optional<Incoming> incoming;
    uint64_t reflexFrameId = 0;
    while (f.active()) {
        sync_reflex();
        sync_affinity();
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
            if (split) f.split.update(b.splitPercent, b.splitSwap);
            record(f, "input", b, nullptr, 0);
            const auto step = history.observe(index, b, next.previous.has_value());
            if (step.stale) {
                record(f, "drop_original_stale", b, nullptr, 0);
                continue;
            }
            if (!step.continuous) {
                cadence.reset();
                lastOriginalTarget = lastPeriod = 0;
                sawRepeat = false;
            }
            incoming = Incoming{b,index,step.continuous};
        }
        const auto b = incoming->frame;
        const auto index = incoming->index;
        const auto continuous = incoming->continuous;
        const auto anchor = history.anchor();
        if (vfg && anchor && (incoming->compareStart || clock_ns() - b.arrivalNs <= 50 * ms)) {
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
            const auto compareNs = clock_ns() - incoming->compareStart;
            record(f, *compared ? "repeat_input" : "content_changed", b, &anchor->frame, 0, compareNs);
            if (*compared) {
                sawRepeat = true;
                incoming.reset();
                if (split) {
                    if (!shown) {
                        PostMessageW(f.window, reveal, 0, 0);
                        shown = true;
                    }
                    refresh_hud(true);
                    const UINT pid = gpu.present(gpu.input[index].Get(), false,
                        static_cast<int>(index), f.split.current(), f.split.swapped());
                    record(f, "present_repeat_split", b, nullptr, pid);
                }
                f.beat();
                continue;
            }
        }
        incoming.reset();
        if (clock_ns() - b.arrivalNs > 50 * ms) {
            record(f, "drop_original_late", b, nullptr, 0);
            history.invalidate();
            cadence.reset();
            lastOriginalTarget = lastPeriod = 0;
            sawRepeat = false;
            continue;
        }
        const auto timestamp = b.obsTick ? b.obsTick : b.timestampNs;
        auto period = vfg ? cadence.observe(timestamp) : 0;
        if (!period && vfg && anchor && (b.obsTick != 0) == (anchor->frame.obsTick != 0) &&
            provisional_60hz(anchor->frame.obsTick ? anchor->frame.obsTick : anchor->frame.timestampNs, timestamp, sawRepeat))
            period = period_60hz;
        if (period != lastPeriod) lastOriginalTarget = 0;
        lastPeriod = period;
        const auto plan = anchor && period ? plan_cadence(b.arrivalNs, lastOriginalTarget, period, mult) : CadencePlan{};
        const auto originalDeadline = plan.originalNs ? plan.originalNs + 5 * ms : b.arrivalNs + 50 * ms;
        const uint64_t fid = ++reflexFrameId;
        reflex.sleep(clock_ns(), clock_ns);
        reflex.marker(ReflexMarker::SimulationStart, fid);
        reflex.marker(ReflexMarker::SimulationEnd, fid);
        reflex.marker(ReflexMarker::RenderSubmitStart, fid);
        if (vfg && anchor && plan.originalNs) {
            const auto& a = anchor->frame;
            if (cudaPending && vfg->subframe_ready(mult - 1)) cudaPending = false;
            const auto stageStart = clock_ns();
            const bool pairLate = stageStart >= plan.synthetic_cutoff(mult - 1);
            const auto staged = !cudaPending && !pairLate ?
                gpu.try_stage(anchor->index, index) : std::nullopt;
            if (staged) {
                record(f, "stage_queued", b, &a, 0, clock_ns() - stageStart);
                const auto enqueueStart = clock_ns();
                vfg->enqueue_subframes(*staged, false);
                record(f, "cuda_enqueued", b, &a, 0, clock_ns() - enqueueStart);
                cudaPending = true;
                reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
                for (uint32_t sub = 1; sub < mult; ++sub) {
                    if (!f.active()) break;
                    const uint64_t targetNs = plan.target(sub);
                    if (clock_ns() < targetNs) pace_until(targetNs, f, paceTimer.h);
                    if (!f.active()) break;
                    const uint64_t waitStart = clock_ns();
                    const auto cutoff = plan.synthetic_cutoff(sub);
                    bool generatedReady = false;
                    while (f.active() && clock_ns() < cutoff) {
                        if (vfg->subframe_ready(sub)) { generatedReady = true; break; }
                        pace_until(std::min<uint64_t>(clock_ns() + 350000, cutoff), f, paceTimer.h);
                    }
                    const auto readyCheck = clock_ns();
                    const uint64_t genElapsed = readyCheck - waitStart;
                    if (!f.active()) break;
                    if (generatedReady && readyCheck < cutoff) {
                        refresh_hud(false);
                        reflex.oob_marker(ReflexMarker::OutOfBandRenderSubmitStart, fid, sub);
                        reflex.oob_marker(ReflexMarker::OutOfBandRenderSubmitEnd, fid, sub);
                        reflex.oob_marker(ReflexMarker::OutOfBandPresentStart, fid, sub);
                        const auto pid = gpu.try_present_synthetic(gpu.exchange[1 + sub].Get(), cutoff,
                            split ? static_cast<int>(index) : -1, split ? f.split.current() : 50, split && f.split.swapped());
                        reflex.oob_marker(ReflexMarker::OutOfBandPresentEnd, fid, sub);
                        record(f, pid ? "present_vfg_subframe" : "drop_vfg_present_busy", b, &a,
                            pid.value_or(0), genElapsed);
                    } else {
                        record(f, "drop_vfg_subframe_late", b, &a, 0,
                            readyCheck > cutoff ? readyCheck - cutoff : 0);
                    }
                }
            } else {
                reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
                record(f, cudaPending ? "drop_vfg_gpu_busy" :
                    pairLate ? "drop_vfg_pair_late" : "drop_vfg_stage_busy", b, &a, 0);
            }
        } else {
            reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
            if (vfg && !continuous)
                record(f, "drop_vfg_discontinuity", b, nullptr, 0);
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
                split ? static_cast<int>(index) : -1, split ? f.split.current() : 50, split && f.split.swapped());
            reflex.marker(ReflexMarker::PresentEnd, fid);
            record(f, "present_original", b, nullptr, pid);
            history.retain(index, b);
            lastOriginalTarget = plan.originalNs;
            f.beat();
        } else {
            record(f, "drop_original_late", b, nullptr, 0);
            history.invalidate();
            cadence.reset();
            lastOriginalTarget = lastPeriod = 0;
            sawRepeat = false;
        }
    }
    f.split.reset(false);
    PostMessageW(f.window, WM_CANCELMODE, 0, 0);
    // Both queues must be idle before releasing their resources or acknowledging
    // a resize. In particular, a successful Present only submits draw work.
    const auto drain_failure = [&] {
        fprintf(f.log, "event=error message=presenter_drain_failed\n");
        ShowWindowAsync(f.window, SW_HIDE);
        TerminateProcess(GetCurrentProcess(), 5);
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    };
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
    drain(gpu.ipcQueue.Get(), 1);
    drain(gpu.drawQueue.Get(), 2);
    try {
        if (vfg) vfg->synchronize(); // Do not acknowledge a failed CUDA drain.
    } catch (const std::exception& e) {
        fprintf(f.log, "event=error message=%s\n", e.what());
        drain_failure();
    }
    vfg.reset();
    if (consumer.close() != ipc::Status::Closed) check(E_FAIL, "IPC drain incomplete");
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
            if (s) InterlockedExchange64(&s->state, -1);
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
            fprintf(file, "event=error message=%s\n", e.what());
            InterlockedExchange64(&view->state, -1);
            feed.result.store(1);
        }
        PostMessageW(window, WM_CLOSE, 0, 0);
    });
    const auto watchdog = SetTimer(window, 1, 200, nullptr);
    MSG msg{};
    while (feed.result.load() < 0 && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            if (feed.stopped() || clock_ns() - feed.heartbeat.load() > 2000 * ms) {
                InterlockedExchange64(&view->state, -1);
                ShowWindow(window, SW_HIDE);
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
    }
    if (watchdog) KillTimer(window, watchdog);
    InterlockedExchange64(&view->state, -1);
    ShowWindow(window, SW_HIDE);
    if (worker.joinable()) worker.join();
    DestroyWindow(window);
    return feed.result.load() == 0 ? 0 : 1;
}
