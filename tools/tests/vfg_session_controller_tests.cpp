// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace nr::fg::vfg;
namespace fs = std::filesystem;

void expect(bool ok, const char* reason)
{
    if (!ok) throw std::runtime_error(reason);
}

struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
};

void check_header()
{
    LaunchOptions opts{};
    expect(opts.backend == Backend::NativeVfg && opts.pushflowTier == PushFlowTier::Fast && opts.multiplier == 2 &&
        !opts.gpuShotChange && !opts.experimentalAnrOptIn && !opts.hudLock && !opts.pacingRepair &&
        !opts.adaptiveFg && !opts.adaptiveTargetFps && !opts.dedupMode && !opts.liveAnchor && !opts.transportIntervalNs,
        "v0.2.12 launch defaults");
    Session s{};
    expect(s.sessionVersion == 3 && s.sessionBytes == sizeof(Session) && !s.dedupMode && !s.liveAnchor &&
        !s.transportIntervalNs && validate_session_header(s), "default v3 session");
    for (const LONG mode : {0L, 1L, 5L, 6L}) {
        s.dedupMode = mode;
        expect(validate_session_header(s), "accept DedupMode v3 header");
    }
    s.dedupMode = 99;
    expect(!validate_session_header(s), "reject dedupMode 99");
    s.dedupMode = 0;
    for (const LONG anchor : {0L, 1L}) {
        s.liveAnchor = anchor;
        expect(validate_session_header(s), "accept liveAnchor v3 header");
    }
    s.liveAnchor = 2;
    expect(!validate_session_header(s), "reject liveAnchor 2");
    s.liveAnchor = 0;
    s.sessionVersion = 99;
    expect(!validate_session_header(s), "reject bad ABI version");
    s.sessionVersion = 2;
    expect(!validate_session_header(s), "reject v2 ABI version");
    s.sessionVersion = 3;
    ++s.sessionBytes;
    expect(!validate_session_header(s), "reject oversized ABI byte count");
    --s.sessionBytes;
    --s.sessionBytes;
    expect(!validate_session_header(s), "reject undersized ABI byte count");
    ++s.sessionBytes;
    std::fill_n(s.pushflowRuntimeDir, ARRAYSIZE(s.pushflowRuntimeDir), L'x');
    expect(!validate_session_header(s), "reject unterminated runtime path");
    s.pushflowRuntimeDir[519] = L'\0';
    expect(validate_session_header(s), "accept 519 characters plus terminator");
    s.backend = Backend::AnrExperimental;
    s.anrDllPath[0] = L'x';
    expect(!validate_session_header(s), "reject ANR without opt-in");
    s.experimentalAnrOptIn = 1;
    expect(validate_session_header(s), "validate opted-in ANR header without loading DLL");
    stamp_exit_reason(s, ExitReason::Fault);
    stamp_exit_reason(s, ExitReason::UserClosed);
    expect(read_exit_reason(s) == ExitReason::Fault, "the first exit reason wins");
    std::printf("PASS vfg_session_controller: session_v3 dedup={0,1,5,6} liveAnchor={0,1} accepted; "
        "sessionVersion={2,99}, dedupMode=99, liveAnchor=2, and mismatched sessionBytes rejected\n");
}

void reject_parent_options()
{
    const auto reject = [](const LaunchOptions& opts, const char* message) {
        try {
            const Controller controller(opts);
        } catch (const nr::fg::ipc::Error& e) {
            expect(std::string(e.what()).find(message) != std::string::npos, "parent error must be the pre-spawn gate");
            return;
        }
        throw std::runtime_error("parent accepted malformed launch");
    };
    LaunchOptions opts{};
    opts.backend = Backend::AnrExperimental;
    opts.anrDllPath = L"unused.dll";
    reject(opts, "VFG invalid session header");
    opts.experimentalAnrOptIn = 1;
    opts.anrDllPath.assign(520, L'x');
    reject(opts, "VFG runtime path too long");
    opts.anrDllPath.clear();
    opts.backend = Backend::PushFlow;
    opts.pushflowRuntimeDir.assign(520, L'x');
    reject(opts, "VFG runtime path too long");
    LaunchOptions badDedup{};
    badDedup.dedupMode = 99;
    reject(badDedup, "VFG invalid session header");
    LaunchOptions badAnchor{};
    badAnchor.liveAnchor = 2;
    reject(badAnchor, "VFG invalid session header");
}

void runtime_contract(const fs::path& scratch)
{
    const auto runtime = (scratch / L"runtime with spaces").lexically_normal();
    fs::create_directories(runtime / L"models");
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", runtime.c_str());
    std::ofstream(runtime / L"onnxruntime.dll") << "discovery only";
    std::ofstream(runtime / L"DirectML.dll") << "discovery only";
    std::ofstream(runtime / L"models" / L"pushflow-fast.onnx") << "discovery only";
    expect(Controller::find_pushflow_dir(PushFlowTier::Fast) == runtime.wstring(), "discover complete Fast tier");
    expect(Controller::find_pushflow_dir(PushFlowTier::Quality) != runtime.wstring(), "wrong tier cannot satisfy runtime gate");
    fs::remove(runtime / L"DirectML.dll");
    expect(Controller::find_pushflow_dir(PushFlowTier::Fast) != runtime.wstring(), "incomplete PushFlow runtime not admitted");
    fs::remove_all(runtime);
}

void runtime_version_order(const fs::path& scratch)
{
    const auto localApp = fs::absolute(scratch / L"veyra-localapp");
    const auto versions = localApp / L"Programs" / L"Veyra (NVIDIA)" / L"versions";
    const auto make_runtime = [&](const wchar_t* version, bool complete) {
        const auto dir = (versions / version / L"runtime" / L"nvidia-vfg").lexically_normal();
        fs::create_directories(dir);
        for (const wchar_t* dll : {L"NVVideoEffects.dll", L"NVCVImage.dll", L"nvVFXVideoFrameGeneration.dll",
                 L"nvngxruntime.dll", L"cudart64_12.dll"}) {
            std::ofstream(dir / dll) << "discovery only";
        }
        if (!complete) fs::remove(dir / L"cudart64_12.dll");
        return dir;
    };
    const auto older = make_runtime(L"2.2.1", true);
    const auto newer = make_runtime(L"2.10.0", true);
    make_runtime(L"2.9.9", false);
    SetEnvironmentVariableW(L"OBS_NR_VFG_RUNTIME", nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", localApp.c_str());
    expect(Controller::find_runtime_dir() == newer.wstring(),
        "Veyra discovery sorts versions numerically and skips incomplete installs");
    std::printf("PASS vfg_session_controller: runtime_version_order 2.10.0 > 2.2.1 (incomplete 2.9.9 skipped)\n");
    fs::remove(newer / L"nvngxruntime.dll");
    expect(Controller::find_runtime_dir() == older.wstring(),
        "next complete Veyra version wins when the newest install lost its DLLs");
    fs::remove_all(localApp);
}

void fallback_live_settings_survive(const fs::path& exe, const fs::path& scratch)
{
    const auto localApp = fs::absolute(scratch / L"live-settings-localapp");
    fs::create_directories(localApp / L"obs-nr-native-filter");
    SetEnvironmentVariableW(L"LOCALAPPDATA", localApp.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", exe.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", (scratch / L"live-settings.log").c_str());
    LaunchOptions opts{};
    opts.backend = Backend::PushFlow;
    opts.multiplier = 2;
    Controller ctrl(opts);
    ctrl.set_dedup_mode(5);
    ctrl.set_live_anchor(1);
    ctrl.set_dedup_mode(99); // invalid live input must not perturb the session
    const auto applied = ctrl.live_settings();
    expect(applied.dedupMode == 5 && applied.liveAnchor == 1,
        "live setters apply dedupMode and liveAnchor to the running session");
    expect(ctrl.begin_async_fallback_relaunch(true), "simulated fallback relaunch begins");
    const auto deadline = clock_ns() + 8'000'000'000ULL;
    while (ctrl.poll_async_fallback_relaunch() != RelaunchState::Ready) {
        expect(clock_ns() < deadline, "simulated fallback relaunch reaches Ready");
        Sleep(10);
    }
    const auto recovered = ctrl.live_settings();
    expect(recovered.dedupMode == 5 && recovered.liveAnchor == 1,
        "dedupMode and liveAnchor survive the fallback recovery relaunch");
    std::printf("PASS vfg_session_controller: live_settings_survive_fallback dedup=5 anchor=1 (invalid dedup=99 ignored)\n");
    ctrl.stop(true);
    expect(ctrl.await_exit_cleanup(2000), "fallback live settings cleanup");
    fs::remove_all(localApp);
}

void malformed_child(const fs::path& exe, const fs::path& log)
{
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    const Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(Session), nullptr)};
    auto* view = static_cast<Session*>(MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, sizeof(Session)));
    expect(view != nullptr, "map malformed child session");
    *view = Session{};
    view->sessionVersion = 99;
    UnmapViewOfFile(view);
    Handle parent;
    expect(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent.value,
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0) != FALSE, "duplicate test parent");
    const auto num = [](HANDLE value) { return std::to_wstring(reinterpret_cast<uintptr_t>(value)); };
    auto cmd = L"\"" + exe.wstring() + L"\" " + num(parent.value) + L" " + num(mapping.value) + L" \"" +
        exe.parent_path().wstring() + L"\" \"" + log.wstring() + L"\" off";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION pi{};
    expect(CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, exe.parent_path().c_str(), &startup, &pi) != FALSE, "launch malformed session fixture");
    const Handle process{pi.hProcess}, thread{pi.hThread};
    const bool exited = WaitForSingleObject(process.value, 3000) == WAIT_OBJECT_0;
    if (!exited) TerminateProcess(process.value, 5);
    DWORD code{};
    expect(exited && GetExitCodeProcess(process.value, &code) && code != 0, "malformed child must exit promptly");
    std::ifstream stream(log);
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    expect(text.find("event=error message=unsupported_session_version") != std::string::npos &&
        text.find("event=initialized ") == std::string::npos, "malformed child rejects before backend init");
}

void cancel_io_ex_and_marker_fallback_contract(const fs::path& exe, const fs::path& scratch)
{
    const std::string bigPayload(262144, 'X');

    // 1a. Normal mode: overlapped WriteFile hits ERROR_IO_PENDING, CancelIoEx cancels it with ERROR_OPERATION_ABORTED.
    {
        const auto pipeName = std::wstring(LR"(\\.\pipe\obs-nr-ctrl-cancel-a-)") + std::to_wstring(GetCurrentProcessId());
        const Handle pipe{CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 64, 64, 0, nullptr)};
        expect(pipe.value != INVALID_HANDLE_VALUE, "create controller cancel pipe 1a");
        const Handle stopPipe{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const Handle connectedPipe{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        std::thread server([&] {
            if (ConnectNamedPipe(pipe.value, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
                SetEvent(connectedPipe.value);
                WaitForSingleObject(stopPipe.value, INFINITE);
            }
        });
        struct ServerJoin {
            HANDLE stop; HANDLE p; std::thread& t;
            ~ServerJoin() { SetEvent(stop); CancelIoEx(p, nullptr); DisconnectNamedPipe(p); if (t.joinable()) t.join(); }
        } serverGuard{stopPipe.value, pipe.value, server};
        Sleep(20);
        const auto t0 = clock_ns();
        const auto ioRes = nr::fg::vfg::detail::write_event_bounded(pipeName, bigPayload, 50);
        const auto elapsedMs = (clock_ns() - t0) / 1'000'000ULL;
        expect(WaitForSingleObject(connectedPipe.value, 1000) == WAIT_OBJECT_0, "server pipe 1a connected");
        expect(ioRes.opened && ioRes.pended && ioRes.cancelled && ioRes.aborted &&
            !ioRes.deferredToPool && !ioRes.timedOut,
            "1a: overlapped write must hit ERROR_IO_PENDING and complete via CancelIoEx with ERROR_OPERATION_ABORTED");
        expect(elapsedMs < 600, "1a: bounded overlapped write must return promptly");
    }

    // 1b. Delayed completion past cancel grace: PendingIoWrite + module stay retained in TP_WAIT until kernel completion.
    {
        const auto pipeName = std::wstring(LR"(\\.\pipe\obs-nr-ctrl-cancel-b-)") + std::to_wstring(GetCurrentProcessId());
        const Handle pipe{CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 64, 64, 0, nullptr)};
        expect(pipe.value != INVALID_HANDLE_VALUE, "create controller cancel pipe 1b");
        const Handle stopPipe{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const Handle connectedPipe{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const Handle reapedEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        expect(stopPipe.value && connectedPipe.value && reapedEvent.value, "create 1b events");
        std::thread server([&] {
            if (ConnectNamedPipe(pipe.value, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
                SetEvent(connectedPipe.value);
                WaitForSingleObject(stopPipe.value, INFINITE);
            }
        });
        struct ServerJoin {
            HANDLE stop; HANDLE p; std::thread& t;
            ~ServerJoin() { SetEvent(stop); CancelIoEx(p, nullptr); DisconnectNamedPipe(p); if (t.joinable()) t.join(); }
        } serverGuard{stopPipe.value, pipe.value, server};
        Sleep(20);
        const auto t0 = clock_ns();
        const nr::fg::vfg::detail::WriteTestHooks hooks{reapedEvent.value, nullptr, false, true};
        const auto ioRes = nr::fg::vfg::detail::write_event_bounded(pipeName, bigPayload, 50, &hooks);
        const auto elapsedMs = (clock_ns() - t0) / 1'000'000ULL;
        expect(WaitForSingleObject(connectedPipe.value, 1000) == WAIT_OBJECT_0, "server pipe 1b connected");
        expect(ioRes.opened && ioRes.pended && !ioRes.cancelled && ioRes.deferredToPool && !ioRes.timedOut,
            "1b: delayed pending write past grace must hand off to TP_WAIT without claiming CancelIoEx");
        expect(elapsedMs < 600, "1b: caller must return bounded without waiting for kernel completion");
        expect(WaitForSingleObject(reapedEvent.value, 30) == WAIT_TIMEOUT,
            "1b: PendingIoWrite must stay retained while kernel WriteFile has not completed");
        SetEvent(stopPipe.value);
        CancelIoEx(pipe.value, nullptr);
        DisconnectNamedPipe(pipe.value);
        server.join();
        expect(WaitForSingleObject(reapedEvent.value, 2000) == WAIT_OBJECT_0,
            "1b: TP_WAIT callback must reap PendingIoWrite after kernel I/O completes");
    }

    // 1c. Forced threadpool-prep failure on a LIVE, OPENABLE pipe server must never call CreateFileW.
    {
        const auto pipeName = std::wstring(LR"(\\.\pipe\obs-nr-ctrl-cancel-c-)") + std::to_wstring(GetCurrentProcessId());
        const Handle pipe{CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT, 2, 64, 64, 0, nullptr)};
        expect(pipe.value != INVALID_HANDLE_VALUE, "create controller cancel pipe 1c");
        const Handle stopPipe{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const Handle connectedPipe{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const Handle connOvEv{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        expect(stopPipe.value && connectedPipe.value && connOvEv.value, "create 1c events");
        std::thread server([&] {
            OVERLAPPED ov{};
            ov.hEvent = connOvEv.value;
            if (!ConnectNamedPipe(pipe.value, &ov)) {
                const auto err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    const HANDLE waits[2]{connOvEv.value, stopPipe.value};
                    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) return;
                } else if (err != ERROR_PIPE_CONNECTED) {
                    return;
                }
            }
            SetEvent(connectedPipe.value);
            WaitForSingleObject(stopPipe.value, INFINITE);
        });
        struct ServerJoin {
            HANDLE stop; HANDLE p; std::thread& t;
            ~ServerJoin() { SetEvent(stop); CancelIoEx(p, nullptr); DisconnectNamedPipe(p); if (t.joinable()) t.join(); }
        } serverGuard{stopPipe.value, pipe.value, server};
        Sleep(20);
        const nr::fg::vfg::detail::WriteTestHooks failHooks{nullptr, nullptr, true, false};
        const auto tFail0 = clock_ns();
        const auto failRes = nr::fg::vfg::detail::write_event_bounded(pipeName, "small", 50, &failHooks);
        const auto failMs = (clock_ns() - tFail0) / 1'000'000ULL;
        expect(!failRes.opened && !failRes.pended && !failRes.deferredToPool && !failRes.timedOut && failMs < 50,
            "1c: threadpool preparation failure must return bounded");
        expect(WaitForSingleObject(connectedPipe.value, 60) == WAIT_TIMEOUT,
            "1c: live pipe server must NOT be connected when threadpool prep fails");
        // Control on the exact same live pipe server: failPoolPrep=false connects and writes cleanly.
        const auto ctrlRes = nr::fg::vfg::detail::write_event_bounded(pipeName, bigPayload, 50);
        expect(WaitForSingleObject(connectedPipe.value, 1000) == WAIT_OBJECT_0,
            "1c control: same live pipe server must connect when prep succeeds");
        expect(ctrlRes.opened && ctrlRes.pended && ctrlRes.cancelled && ctrlRes.aborted,
            "1c control: live pipe write succeeds up to ERROR_IO_PENDING/CancelIoEx");
    }

    // 1d. Stalled diagnostic open/write in TP_WORK must NOT block write_event_bounded, Controller cleanup, destruction, or reopen.
    // Also verify zero OS handle leaks across repeated normal diagnostic writes.
    {
        const auto diagLog = scratch / L"ctrl-diag-stall.log";
        fs::remove(diagLog);
        const Handle stallEv{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const Handle reapEv{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        expect(stallEv.value && reapEv.value, "create 1d direct stall events");
        struct StallSignalGuard {
            HANDLE ev{};
            ~StallSignalGuard() { if (ev) SetEvent(ev); }
        } stallGuard{stallEv.value};
        const nr::fg::vfg::detail::WriteTestHooks stallHooks{reapEv.value, stallEv.value, false, false};
        const auto t0 = clock_ns();
        const auto st = nr::fg::vfg::detail::write_event_bounded(diagLog.wstring(), "event=direct_stall_probe\n", 50, &stallHooks);
        const auto callMs = (clock_ns() - t0) / 1'000'000ULL;
        expect(st.timedOut && callMs < 500, "1d direct: stalled open/write returns bounded with timedOut=true");
        expect(WaitForSingleObject(reapEv.value, 20) == WAIT_TIMEOUT,
            "1d direct: TP_WORK retains state until stalled open/write completes");
        SetEvent(stallEv.value);
        expect(WaitForSingleObject(reapEv.value, 2000) == WAIT_OBJECT_0,
            "1d direct: TP_WORK completes and signals reaped after stall clears");
        const auto txt = [&] { std::ifstream s(diagLog); return std::string(std::istreambuf_iterator<char>(s), {}); }();
        expect(txt.find("event=direct_stall_probe") != std::string::npos,
            "1d direct: deferred write wrote payload once stall cleared");

        // Repeated normal diagnostic writes must not leak file/event/threadpool handles.
        DWORD handlesBeforeLoop = 0, handlesAfterLoop = 0;
        expect(GetProcessHandleCount(GetCurrentProcess(), &handlesBeforeLoop) != FALSE, "query handles before normal loop");
        for (int i = 0; i < 16; ++i) {
            const Handle loopReap{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            const nr::fg::vfg::detail::WriteTestHooks loopHooks{loopReap.value, nullptr, false, false};
            const auto ws = nr::fg::vfg::detail::write_event_bounded(diagLog.wstring(), "event=handle_leak_probe\n", 150, &loopHooks);
            expect(ws.opened && !ws.timedOut && !ws.deferredToPool, "normal write succeeds cleanly");
            expect(WaitForSingleObject(loopReap.value, 1000) == WAIT_OBJECT_0, "normal write reaps handles");
        }
        Sleep(30);
        expect(GetProcessHandleCount(GetCurrentProcess(), &handlesAfterLoop) != FALSE, "query handles after normal loop");
        expect(handlesAfterLoop <= handlesBeforeLoop + 2,
            "repeated diagnostic write_event_bounded calls must not leak OS handles");
    }

    const auto localApp = scratch / L"ctrl-localapp";
    const auto marks = localApp / L"obs-nr-native-filter";
    fs::remove_all(localApp);
    fs::create_directories(marks);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", exe.c_str());
    {
        // Unset LOCALAPPDATA so PushFlow Controller observer deterministically fails marker cleanup
        // and invokes detail::write_event_bounded(s->logPath, diag, 150) on stop(true).
        SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
        const auto diagLog = scratch / L"ctrl-controller-stall.log";
        fs::remove(diagLog);
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", diagLog.c_str());
        const auto stallName = LR"(Local\obs-nr-diag-stall-)" + std::to_wstring(GetCurrentProcessId());
        const auto reapName = LR"(Local\obs-nr-diag-reap-)" + std::to_wstring(GetCurrentProcessId());
        const Handle stallEv{CreateEventW(nullptr, TRUE, FALSE, stallName.c_str())};
        const Handle reapEv{CreateEventW(nullptr, TRUE, FALSE, reapName.c_str())};
        expect(stallEv.value && reapEv.value, "create 1d controller diag stall events");
        struct EnvClear {
            HANDLE ev{};
            ~EnvClear()
            {
                if (ev) SetEvent(ev);
                SetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_STALL_EVENT", nullptr);
                SetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_REAPED_EVENT", nullptr);
            }
        } envClear{stallEv.value};
        SetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_STALL_EVENT", stallName.c_str());
        SetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_REAPED_EVENT", reapName.c_str());

        const auto t0 = clock_ns();
        {
            LaunchOptions opts{};
            opts.backend = Backend::PushFlow;
            opts.multiplier = 2;
            auto ctrl = std::make_unique<Controller>(opts);
            ctrl->stop(true);
            expect(ctrl->await_exit_cleanup(1800), "1d: await_exit_cleanup must remain bounded while diag open/write stalls");
            const auto tDestroy0 = clock_ns();
            ctrl.reset();
            const auto destroyMs = (clock_ns() - tDestroy0) / 1'000'000ULL;
            expect(destroyMs < 600, "1d: Controller destruction must not block on stalled diag open/write");
        }
        expect(WaitForSingleObject(reapEv.value, 20) == WAIT_TIMEOUT,
            "1d: observer diagnostic TP_WORK state stays retained while stalled");
        SetEnvironmentVariableW(L"LOCALAPPDATA", localApp.c_str());
        {
            const auto tReopen0 = clock_ns();
            LaunchOptions reopenOpts{};
            reopenOpts.backend = Backend::PushFlow;
            reopenOpts.multiplier = 2;
            Controller reopenCtrl(reopenOpts);
            const auto reopenMs = (clock_ns() - tReopen0) / 1'000'000ULL;
            expect(reopenMs < 600, "1d: new preview Controller launch must remain bounded while prior diag write is stalled");
            reopenCtrl.stop(true);
            expect(reopenCtrl.await_exit_cleanup(1500), "1d: reopen cleanup");
        }
        SetEvent(stallEv.value);
        expect(WaitForSingleObject(reapEv.value, 2000) == WAIT_OBJECT_0,
            "1d: observer diag TP_WORK must finish and release module/state once stall clears");
        const auto diagText = [&] { std::ifstream s(diagLog); return std::string(std::istreambuf_iterator<char>(s), {}); }();
        expect(diagText.find("event=pushflow_marker_cleanup_failed") != std::string::npos,
            "1d: deferred observer diag write completed cleanly to log after stall cleared");
        const auto totalMs = (clock_ns() - t0) / 1'000'000ULL;
        expect(totalMs < 10000, "1d total bounded");
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", localApp.c_str());

    // Kernel-object cross-host token isolation check:
    // While another host holds Local\obs-nr-pf-tok-<extTok>, our Controller MUST preserve its starting marker;
    // once that external mutex closes (host died), the next Controller MUST treat it as a crash marker and consume it.
    {
        constexpr uint64_t extTok = 0x0000FEDCBA987654ULL;
        const auto extMarker = marks / nr::fg::vfg::starting_marker_filename(extTok);
        const auto extMutexName = nr::fg::vfg::marker_mutex_name(extTok);
        const auto xlog = scratch / L"ctrl-xhost.log";
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", xlog.c_str());
        {
            Handle extLock{CreateMutexW(nullptr, FALSE, extMutexName.c_str())};
            expect(extLock.value != nullptr, "create simulated external host mutex");
            std::ofstream(extMarker) << "pushflow_start_or_teardown\n";
            fs::remove(xlog);
            LaunchOptions opts{};
            opts.backend = Backend::PushFlow;
            opts.multiplier = 2;
            Controller ctrl(opts);
            expect(fs::exists(extMarker), "live external host starting marker must NOT be deleted by Controller");
            const auto logTxt = [&] { std::ifstream s(xlog); return std::string(std::istreambuf_iterator<char>(s), {}); }();
            expect(logTxt.find("event=pushflow_previous_crash_detected") == std::string::npos,
                "live external host marker must NOT trigger crash degradation in our Controller");
            ctrl.stop(true);
            expect(ctrl.await_exit_cleanup(1500), "cleanup after live external marker check");
        }
        // extLock is now closed (external host exited without cleaning extMarker) -> next Controller must detect crash & consume extMarker.
        {
            fs::remove(xlog);
            LaunchOptions opts{};
            opts.backend = Backend::PushFlow;
            opts.multiplier = 2;
            Controller ctrl(opts);
            expect(!fs::exists(extMarker), "dead external host starting marker must be consumed on crash detection");
            const auto logTxt = [&] { std::ifstream s(xlog); return std::string(std::istreambuf_iterator<char>(s), {}); }();
            expect(logTxt.find("event=pushflow_previous_crash_detected") != std::string::npos,
                "dead external host starting marker must trigger crash degradation");
            ctrl.stop(true);
            expect(ctrl.await_exit_cleanup(1500), "cleanup after dead external marker check");
        }
    }

    // 2. Pending vs Succeeded marker status, cross-token fallback isolation, and fail-closed markers.
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", (scratch / L"ctrl-marker.log").c_str());
    {
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.multiplier = 2;
        Controller ctrl(opts);
        expect(!ctrl.user_close_marker_cleaned(), "active PushFlow cleanup status must start Pending, not Succeeded");
        ctrl.stop(true);
        expect(ctrl.await_exit_cleanup(1500) && ctrl.user_close_marker_cleaned(),
            "UserClosed PushFlow settles cleanup status to Succeeded");
    }
    // Cross-token isolation: fallback for token 0000000000001234 must NOT remove token 0000000000005678's marker.
    const auto tok1234 = marks / L"pushflow-starting-0000000000001234.txt";
    const auto tok5678 = marks / L"pushflow-starting-0000000000005678.txt";
    const auto fallback = marks / L"pushflow-user-closed.txt";
    {
        std::ofstream(tok1234) << "token=0000000000001234\n";
        std::ofstream(tok5678) << "token=0000000000005678\n";
        std::ofstream(fallback) << "token=0000000000001234\n";
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.multiplier = 2;
        Controller ctrl(opts);
        expect(!fs::exists(tok1234) && !fs::exists(tok5678) && !fs::exists(fallback),
            "dead unowned token 5678 triggers crash handling while token 1234 is cleared");
        const auto ctrlText = [&] { std::ifstream s(scratch / L"ctrl-marker.log"); return std::string(std::istreambuf_iterator<char>(s), {}); }();
        expect(ctrlText.find("event=pushflow_previous_crash_detected") != std::string::npos,
            "cross-token leftover starting marker must still trigger crash degradation");
        ctrl.stop(true);
    }
    // Stage token=0000000000001234 fallback + locked tok1234 -> next Controller must NOT drop fallback when remove fails.
    {
        std::ofstream(tok1234) << "token=0000000000001234\n";
        std::ofstream(fallback) << "token=0000000000001234\n";
    }
    {
        const Handle locked{CreateFileW(tok1234.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr)};
        expect(locked.value != INVALID_HANDLE_VALUE, "lock starting marker without FILE_SHARE_DELETE");
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.multiplier = 2;
        Controller ctrl(opts);
        expect(fs::exists(fallback), "fallback marker must remain if starting marker removal failed");
        ctrl.stop(true);
    }
    // Unlock and let next Controller consume both markers cleanly.
    {
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.multiplier = 2;
        Controller ctrl(opts);
        expect(!fs::exists(tok1234) && !fs::exists(fallback),
            "next Controller consumes user-closed fallback and matching starting marker together");
        ctrl.stop(true);
    }
    // Corrupt/truncated fallback must NOT clear starting marker and must degrade to 1x.
    const auto starting = marks / L"pushflow-starting.txt";
    {
        std::ofstream(starting) << "real_crash_marker\n";
        std::ofstream(fallback) << "reason=trunc";
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.multiplier = 2;
        Controller ctrl(opts);
        expect(!fs::exists(fallback), "corrupt fallback marker is discarded");
        const auto ctrlText = [&] { std::ifstream s(scratch / L"ctrl-marker.log"); return std::string(std::istreambuf_iterator<char>(s), {}); }();
        expect(ctrlText.find("event=pushflow_fallback_corrupt") != std::string::npos &&
            ctrlText.find("event=pushflow_previous_crash_detected") != std::string::npos,
            "corrupt fallback triggers crash degradation instead of clearing starting marker");
        ctrl.stop(true);
    }
    // Locked fallback (remove failure) must degrade to 1x instead of launching 2x PushFlow with stale fallback.
    {
        std::ofstream(starting) << "prior_starting\n";
        std::ofstream(fallback) << "reason=user_closed_marker_cleanup_failed\n";
        const Handle lockedFb{CreateFileW(fallback.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr)};
        expect(lockedFb.value != INVALID_HANDLE_VALUE, "lock fallback marker without FILE_SHARE_DELETE");
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.multiplier = 2;
        Controller ctrl(opts);
        const auto ctrlText = [&] { std::ifstream s(scratch / L"ctrl-marker.log"); return std::string(std::istreambuf_iterator<char>(s), {}); }();
        expect(ctrlText.find("event=pushflow_fallback_remove_failed") != std::string::npos &&
            ctrlText.find("event=pushflow_previous_crash_detected") != std::string::npos,
            "unremovable fallback marker forces 1x degradation");
        ctrl.stop(true);
    }

    // 3. Persistent CreateMutexW failure or bounded collision exhaustion must NOT spin indefinitely or launch unprotected 2x:
    //    must degrade explicitly to 1x original mode in bounded time with zero OS handle leak.
    {
        static std::atomic_int mutexCalls{0};
        struct MutexHookGuard {
            ~MutexHookGuard() { nr::fg::vfg::detail::set_marker_mutex_create_fn_for_test(nullptr); }
        } hookGuard;

        // 3a. Non-collision failure (nullptr + ERROR_NO_SYSTEM_RESOURCES) must break immediately on attempt 1 (no retry spin).
        mutexCalls.store(0);
        nr::fg::vfg::detail::set_marker_mutex_create_fn_for_test([](LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR) -> HANDLE {
            mutexCalls.fetch_add(1, std::memory_order_relaxed);
            SetLastError(ERROR_NO_SYSTEM_RESOURCES);
            return nullptr;
        });
        const auto failLog = scratch / L"ctrl-mutex-fail.log";
        fs::remove(failLog);
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", failLog.c_str());
        {
            const auto t0 = clock_ns();
            LaunchOptions opts{};
            opts.backend = Backend::PushFlow;
            opts.multiplier = 2;
            Controller ctrl(opts);
            const auto ms = (clock_ns() - t0) / 1'000'000ULL;
            expect(mutexCalls.load() == 1 && ms < 1000,
                "3a: non-collision CreateMutexW failure must stop immediately without retry spin");
            expect(ctrl.alive(), "3a: fallback 1x presenter launched cleanly");
            ctrl.stop(true);
            expect(ctrl.await_exit_cleanup(1500), "3a: controller cleanup after 1x fallback");
        }
        const auto failTxt = [&] { std::ifstream s(failLog); return std::string(std::istreambuf_iterator<char>(s), {}); }();
        expect(failTxt.find("event=pushflow_marker_lock_failed_fallback_1x") != std::string::npos &&
            failTxt.find("event=session_started backend=pushflow") != std::string::npos &&
            failTxt.find("mode=2x") == std::string::npos,
            "3a: CreateMutexW failure degrades explicitly to 1x without starting unprotected 2x");

        // 3b. Persistent ERROR_ALREADY_EXISTS collision must be bounded at 64 retries, close duplicate handles, and degrade to 1x.
        mutexCalls.store(0);
        DWORD handlesBefore = 0, handlesAfter = 0;
        expect(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != FALSE, "query handles before 3b");
        nr::fg::vfg::detail::set_marker_mutex_create_fn_for_test([](LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR) -> HANDLE {
            mutexCalls.fetch_add(1, std::memory_order_relaxed);
            const HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            SetLastError(ERROR_ALREADY_EXISTS);
            return h;
        });
        const auto collLog = scratch / L"ctrl-mutex-collision.log";
        fs::remove(collLog);
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", collLog.c_str());
        {
            const auto t0 = clock_ns();
            LaunchOptions opts{};
            opts.backend = Backend::PushFlow;
            opts.multiplier = 2;
            Controller ctrl(opts);
            const auto ms = (clock_ns() - t0) / 1'000'000ULL;
            expect(mutexCalls.load() == 64 && ms < 1000,
                "3b: ERROR_ALREADY_EXISTS retry loop must be bounded at 64 attempts");
            expect(ctrl.alive(), "3b: fallback 1x presenter launched cleanly");
            ctrl.stop(true);
            expect(ctrl.await_exit_cleanup(1500), "3b: controller cleanup after collision exhaustion");
        }
        expect(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != FALSE, "query handles after 3b");
        expect(handlesAfter <= handlesBefore + 2,
            "3b: bounded collision retries must close existing handles without leaking");
        const auto collTxt = [&] { std::ifstream s(collLog); return std::string(std::istreambuf_iterator<char>(s), {}); }();
        expect(collTxt.find("event=pushflow_marker_lock_failed_fallback_1x") != std::string::npos &&
            collTxt.find("event=session_started backend=pushflow") != std::string::npos &&
            collTxt.find("mode=2x") == std::string::npos,
            "3b: collision exhaustion degrades explicitly to 1x without starting unprotected 2x");
    }
    fs::remove_all(localApp);
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        expect(argc == 3, "arguments: presenter scratch");
        const fs::path exe(argv[1]), scratch = fs::path(argv[2]) / std::to_wstring(GetCurrentProcessId());
        fs::create_directories(scratch);
        const auto log = scratch / L"malformed-session.log";
        fs::remove(log);
        check_header();
        SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", (scratch / L"must-not-run.exe").c_str());
        reject_parent_options();
        runtime_contract(scratch);
        runtime_version_order(scratch);
        malformed_child(exe, log);
        cancel_io_ex_and_marker_fallback_contract(exe, scratch);
        fallback_live_settings_survive(exe, scratch);
        std::puts("PASS vfg_session_controller: v3 defaults, malformed header and paths, parent and child admission, "
            "numeric runtime order, overlapped cancel & marker fallback, live dedup/anchor across fallback");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_session_controller: %s\n", e.what());
        return 1;
    }
}
