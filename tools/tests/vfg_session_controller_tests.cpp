// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

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
        !opts.adaptiveFg && !opts.adaptiveTargetFps, "v0.2.10 launch defaults");
    Session s{};
    expect(s.sessionVersion == 2 && s.sessionBytes == sizeof(Session) && validate_session_header(s),
        "default v2 session");
    s.sessionVersion = 99;
    expect(!validate_session_header(s), "reject bad ABI version");
    s.sessionVersion = 2;
    --s.sessionBytes;
    expect(!validate_session_header(s), "reject bad ABI byte count");
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

    // 1a. Normal mode: overlapped WriteFile hits ERROR_IO_PENDING, CancelIoEx cancels it within 50ms grace.
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
        expect(ioRes.opened && ioRes.pended && ioRes.cancelled && !ioRes.deferredToPool,
            "1a: overlapped write must hit ERROR_IO_PENDING and complete via CancelIoEx");
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
        const auto ioRes = nr::fg::vfg::detail::write_event_bounded(pipeName, bigPayload, 50, reapedEvent.value, false, true);
        const auto elapsedMs = (clock_ns() - t0) / 1'000'000ULL;
        expect(WaitForSingleObject(connectedPipe.value, 1000) == WAIT_OBJECT_0, "server pipe 1b connected");
        expect(ioRes.opened && ioRes.pended && ioRes.cancelled && ioRes.deferredToPool,
            "1b: pending write past cancel grace must hand off to TP_WAIT");
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

    // 1c. Forced threadpool-prep failure must return immediately without INFINITE wait.
    const auto tFail0 = clock_ns();
    const auto failRes = nr::fg::vfg::detail::write_event_bounded(L"\\\\.\\pipe\\unused", "ignored", 50, nullptr, true, false);
    const auto failMs = (clock_ns() - tFail0) / 1'000'000ULL;
    expect(!failRes.opened && !failRes.pended && !failRes.deferredToPool && failMs < 50,
        "1c: threadpool wait preparation failure must return bounded without issuing WriteFile");

    // 2. Pending vs Succeeded marker status and UserClosed fallback consumption on locked/corrupt markers.
    const auto localApp = scratch / L"ctrl-localapp";
    const auto marks = localApp / L"obs-nr-native-filter";
    fs::remove_all(localApp);
    fs::create_directories(marks);
    SetEnvironmentVariableW(L"LOCALAPPDATA", localApp.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", exe.c_str());
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
    // Stage pushflow-user-closed.txt + locked pushflow-starting.txt -> next Controller must NOT drop fallback when remove fails.
    const auto starting = marks / L"pushflow-starting.txt";
    const auto fallback = marks / L"pushflow-user-closed.txt";
    {
        std::ofstream(starting) << "locked\n";
        std::ofstream(fallback) << "reason=user_closed_marker_cleanup_failed\n";
    }
    {
        const Handle locked{CreateFileW(starting.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
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
        expect(!fs::exists(starting) && !fs::exists(fallback),
            "next Controller consumes user-closed fallback and starting marker together");
        ctrl.stop(true);
    }
    // Corrupt/truncated fallback must NOT clear starting marker and must degrade to 1x.
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
        malformed_child(exe, log);
        cancel_io_ex_and_marker_fallback_contract(exe, scratch);
        std::puts("PASS vfg_session_controller: v2 defaults, malformed header and paths, parent and child admission, overlapped cancel & marker fallback");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_session_controller: %s\n", e.what());
        return 1;
    }
}
