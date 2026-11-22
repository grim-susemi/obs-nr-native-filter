// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

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
        std::puts("PASS vfg_session_controller: v2 defaults, malformed header and paths, parent and child admission");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_session_controller: %s\n", e.what());
        return 1;
    }
}
