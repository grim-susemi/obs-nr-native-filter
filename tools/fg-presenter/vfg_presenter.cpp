// SPDX-License-Identifier: GPL-3.0-only
#include "native_vfg.h"
#include "nr_fps_hud.h"
#include "vfg_reflex.h"
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
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

struct Feed {
    Session& view;
    HANDLE parent;
    HWND window;
    FILE* log;
    const wchar_t* runtime;
    uint32_t cliMultiplier;
    std::atomic_uint64_t heartbeat{clock_ns()};
    std::atomic_int result{-1};
    bool stopped() { return state(view) < 0 || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT; }
    void beat() { heartbeat.store(clock_ns()); }
};

void record(Feed& f, const char* kind, const ipc::Frame& b, const ipc::Frame* a, UINT id, uint64_t workNs = 0)
{
    const auto q = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    fprintf(f.log, "event=%s generation=%llu b=%llu b_qpc_ns=%llu b_arrival_ns=%llu a=%llu a_qpc_ns=%llu present_id=%u qpc_ns=%llu work_ns=%llu\n",
        kind, q(b.generation), q(b.serial), q(b.timestampNs), q(b.arrivalNs), q(a ? a->serial : 0),
        q(a ? a->timestampNs : 0), id, q(clock_ns()), q(workNs));
}

void pace_until(uint64_t targetNs, Feed& f)
{
    for (;;) {
        if (f.stopped()) return;
        const uint64_t now = clock_ns();
        if (now >= targetNs) return;
        const uint64_t remMs = (targetNs - now) / ms;
        if (remMs >= 2) Sleep(1);
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
    while (state(f.view) == 0 && !f.stopped()) Sleep(1);
    if (f.stopped()) return;
    f.beat();
    if (f.view.width < 320 || f.view.height < 180 || f.view.width > 7680 || f.view.height > 4320 ||
        !f.view.generation || !f.view.mapping[0])
        check(E_INVALIDARG, "unsupported session offer");
    const uint32_t mult = f.cliMultiplier <= 1 ? 1 : std::clamp<uint32_t>(f.view.multiplier ? f.view.multiplier : f.cliMultiplier, 2, max_multiplier);
    fprintf(f.log, "event=offer mode=%ux size=%ux%u quality=%u generation=%llu luid=%08lx:%08lx hwnd=%p pid=%lu\n",
        mult, f.view.width, f.view.height, f.view.quality,
        static_cast<unsigned long long>(f.view.generation), static_cast<unsigned long>(f.view.adapter.HighPart),
        f.view.adapter.LowPart, static_cast<void*>(f.window), GetCurrentProcessId());
    set_gpu_process_priority(InterlockedCompareExchange(&f.view.gpuPriority, 0, 0) != 0, f.log);
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
    ipc::Consumer consumer(gpu.ipcQueue.Get(), {f.view.mapping, f.view.generation}, ipc::identity(f.parent));
    std::unique_ptr<NativeVfg> vfg;
    if (mult > 1) vfg = std::make_unique<NativeVfg>(f.runtime, gpu, f.view);
    fprintf(f.log, "event=initialized backend=%s sdk_version=0x%08X low_latency=%ld\n",
        vfg ? "nvidia-vfg-native" : "original", vfg ? vfg->sdk_version() : 0u, lastReflex);
    f.beat();
    bool shown = false;
    unsigned nextInput = 0;
    uint64_t newestAccepted = 0;
    uint64_t lastOriginal = 0;
    uint64_t hudWindowStart = clock_ns();
    uint64_t hudOrigFrames = 0, hudPresentedFrames = 0;
    double hudInFps = 0.0, hudOutFps = 0.0;
    uint32_t lastHudPos = UINT32_MAX;
    std::string lastHudKey;
    auto refresh_hud = [&](bool isOriginal) {
        ++hudPresentedFrames;
        if (isOriginal) ++hudOrigFrames;
        const uint64_t now = clock_ns();
        const uint64_t dt = now - hudWindowStart;
        const uint32_t pos = static_cast<uint32_t>(std::clamp<LONG>(InterlockedCompareExchange(&f.view.fpsOverlay, 0, 0), 0, 2));
        if (dt >= 250 * ms) {
            const double sec = static_cast<double>(dt) / 1e9;
            hudInFps = static_cast<double>(hudOrigFrames) / sec;
            hudOutFps = static_cast<double>(hudPresentedFrames) / sec;
            hudOrigFrames = 0;
            hudPresentedFrames = 0;
            hudWindowStart = now;
        }
        if (pos == 0) {
            if (lastHudPos != 0) { gpu.set_hud(0, 0, 0, nullptr); lastHudPos = 0; lastHudKey.clear(); }
            return;
        }
        char left[32]{}, right[48]{};
        std::snprintf(left, sizeof(left), "ORIG %5.1f", hudInFps);
        if (mult > 1) std::snprintf(right, sizeof(right), "VFG %5.1f FPS (%uX)", hudOutFps, mult);
        else std::snprintf(right, sizeof(right), "OFF %5.1f FPS", hudOutFps);
        const std::string key = std::string(left) + "|" + right + "|" + std::to_string(pos);
        if (key != lastHudKey) {
            const uint32_t scale = f.view.height >= 1440 ? 3u : 2u;
            const auto accent = mult > 1 ? nr::hud::rgba(0x3D, 0xDC, 0x84, 0xFF) : nr::hud::rgba(0xFF, 0xB8, 0x4D, 0xFF);
            const auto img = nr::hud::render_fps_pill(left, " -> ", right, scale, 255, accent);
            gpu.set_hud(pos, img.width, img.height, img.pixels.data());
            lastHudPos = pos;
            lastHudKey = key;
        }
    };
    uint64_t reflexFrameId = 0;
    while (!f.stopped()) {
        sync_reflex();
        const auto index = nextInput;
        const auto next = consumer.copy_next(gpu.input[index].Get());
        if (!next.current) {
            if (next.status != ipc::Status::Pending && next.status != ipc::Status::Dropped)
                check(E_FAIL, "IPC copy_next");
            WaitForSingleObject(consumer.frame_event(), 4);
            continue;
        }
        if (next.status != ipc::Status::Ok) check(E_FAIL, "IPC copy submit");
        const auto& b = next.current->frame;
        const auto deadline = clock_ns() + 200 * ms;
        auto ready = [&](const ipc::CopyTicket& t) {
            for (;;) {
                const auto status = consumer.copy_status(t);
                if (status == ipc::Status::Ok) return;
                if (status != ipc::Status::Pending || clock_ns() >= deadline) check(E_FAIL, "IPC copy completion");
                Sleep(0);
            }
        };
        ready(*next.current);
        record(f, "input", b, nullptr, 0);
        if (f.stopped()) break;
        if (b.serial <= newestAccepted) {
            record(f, "drop_original_stale", b, nullptr, 0);
            continue;
        }
        newestAccepted = b.serial;
        nextInput = index ^ 1;
        if (clock_ns() - b.arrivalNs > 50 * ms) {
            record(f, "drop_original_late", b, nullptr, 0);
            continue;
        }
        const uint64_t fid = ++reflexFrameId;
        reflex.sleep(clock_ns(), clock_ns);
        reflex.marker(ReflexMarker::SimulationStart, fid);
        reflex.marker(ReflexMarker::SimulationEnd, fid);
        reflex.marker(ReflexMarker::RenderSubmitStart, fid);
        if (vfg && next.previous) {
            const auto& a = next.previous->frame;
            if (a.serial == lastOriginal && b.timestampNs > a.timestampNs && b.timestampNs - a.timestampNs <= 25 * ms) {
                ready(*next.previous);
                const uint64_t pairInterval = b.timestampNs - a.timestampNs;
                const uint64_t pairStart = clock_ns();
                gpu.stage(index ^ 1, index);
                vfg->enqueue_subframes(false);
                reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
                for (uint32_t sub = 1; sub < mult; ++sub) {
                    if (f.stopped()) break;
                    if (WaitForSingleObject(consumer.frame_event(), 0) == WAIT_OBJECT_0 ||
                        clock_ns() - b.arrivalNs > 28 * ms) {
                        record(f, "drop_vfg_subframe_late", b, &a, 0, 0);
                        break;
                    }
                    const uint64_t targetNs = pairStart + (pairInterval * sub) / mult;
                    if (clock_ns() < targetNs) pace_until(targetNs, f);
                    if (f.stopped()) break;
                    const uint64_t waitStart = clock_ns();
                    vfg->wait_subframe(sub);
                    const uint64_t genElapsed = clock_ns() - waitStart;
                    if (clock_ns() - b.arrivalNs <= 40 * ms) {
                        refresh_hud(false);
                        reflex.oob_marker(ReflexMarker::OutOfBandRenderSubmitStart, fid, sub);
                        reflex.oob_marker(ReflexMarker::OutOfBandRenderSubmitEnd, fid, sub);
                        reflex.oob_marker(ReflexMarker::OutOfBandPresentStart, fid, sub);
                        const UINT pid = gpu.present(gpu.exchange[1 + sub].Get(), true);
                        reflex.oob_marker(ReflexMarker::OutOfBandPresentEnd, fid, sub);
                        record(f, "present_vfg_subframe", b, &a, pid, genElapsed);
                    } else {
                        record(f, "drop_vfg_subframe_late", b, &a, 0, genElapsed);
                    }
                }
            } else {
                reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
                record(f, "drop_vfg_discontinuity", b, &a, 0);
            }
        } else {
            reflex.marker(ReflexMarker::RenderSubmitEnd, fid);
        }
        if (f.stopped()) break;
        if (clock_ns() - b.arrivalNs <= 50 * ms) {
            if (!shown) {
                PostMessageW(f.window, reveal, 0, 0);
                shown = true;
            }
            refresh_hud(true);
            reflex.marker(ReflexMarker::PresentStart, fid);
            const UINT pid = gpu.present(gpu.input[index].Get(), false);
            reflex.marker(ReflexMarker::PresentEnd, fid);
            record(f, "present_original", b, nullptr, pid);
            lastOriginal = b.serial;
            f.beat();
        } else {
            record(f, "drop_original_late", b, nullptr, 0);
        }
    }
    consumer.close();
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

    FILE* file{};
    if (_wfopen_s(&file, argv[4], L"ab") != 0) return 2;
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
        if (msg == reveal) {
            auto* s = reinterpret_cast<Session*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (s && state(*s) == 1) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            return 0;
        }
        if (msg == WM_CLOSE || (msg == WM_KEYDOWN && wp == VK_ESCAPE)
#ifdef OBS_NR_VFG_QA_ACTION
            || msg == WM_DISPLAYCHANGE
#endif
        ) {
            auto* s = reinterpret_cast<Session*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
    if (!SetWindowDisplayAffinity(window, 0x11)) { DestroyWindow(window); return 2; }
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));

    Feed feed{*view, parent.h, window, file, argv[3], cliMult};
    std::thread worker([&] {
        try { frames(feed); feed.result.store(0); }
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
        if (msg.message == WM_TIMER && (feed.stopped() || clock_ns() - feed.heartbeat.load() > 2000 * ms)) {
            InterlockedExchange64(&view->state, -1);
            ShowWindow(window, SW_HIDE);
            break;
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
