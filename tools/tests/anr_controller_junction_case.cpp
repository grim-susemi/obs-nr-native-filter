// SPDX-License-Identifier: GPL-3.0-only
#include "fg/fg_controller.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace nr::fg::vfg;
unsigned dispatches{};

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string controller_error(const LaunchOptions& options)
{
    try {
        const Controller controller(options);
    } catch (const nr::fg::ipc::Error& error) {
        require(error.status() == nr::fg::ipc::Status::ProtocolError, "unexpected Controller error status");
        return error.what();
    }
    throw std::runtime_error("Controller unexpectedly returned");
}
} // namespace

BOOL WINAPI observe_presenter_dispatch(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION)
{
    ++dispatches;
    SetLastError(ERROR_CANCELLED);
    return FALSE;
}

bool anr_controller_junction_case(const std::filesystem::path& root)
{
    wchar_t exe[32768]{}, systemDir[MAX_PATH]{};
    const auto exeLen = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    const auto systemLen = GetSystemDirectoryW(systemDir, ARRAYSIZE(systemDir));
    require(exeLen && exeLen < ARRAYSIZE(exe) && systemLen && systemLen < ARRAYSIZE(systemDir), "resolve test paths");
    require(SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", exe) &&
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", (root / L"controller.log").c_str()), "set owned test paths");

    LaunchOptions options{};
    options.backend = Backend::AnrExperimental;
    options.experimentalAnrOptIn = 1;
    options.anrDllPath = (root / L"addon" / L"nvngx_dlssg.dll").native();
    // The ordinary incomplete addon proceeds to the launch call, which the probe stops.
    const auto control = controller_error(options);
    std::cout << "dispatch control: reason=" << control << " dispatch_calls=" << dispatches << '\n';
    require(control == "VFG start presenter" && dispatches == 1, "dispatch probe did not observe the positive control");
    dispatches = 0;

    fs::create_directory(root / L"addon" / L"programs");
    fs::create_directory(root / L"junction-target");
    std::ofstream pack;
    pack.exceptions(std::ios::failbit | std::ios::badbit);
    pack.open(root / L"junction-target" / L"pack.nrx", std::ios::binary);
    pack << "owned inert companion\n";
    pack.close();
    const auto junction = root / L"addon" / L"programs" / L"fg-lite-any-dp4a-srgb8";
    struct JunctionCleanup {
        const fs::path& entry;
        ~JunctionCleanup() { if (!removed) RemoveDirectoryW(entry.c_str()); }
        bool removed{};
    } cleanup{junction};
    const auto cmd = fs::path(systemDir) / L"cmd.exe";
    wchar_t command[] = LR"(cmd.exe /d /v:off /c mklink /j "addon\programs\fg-lite-any-dp4a-srgb8" "junction-target")";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    require(CreateProcessW(cmd.c_str(), command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(),
        &startup, &child), "start Windows junction creation utility");
    CloseHandle(child.hThread);
    const auto wait = WaitForSingleObject(child.hProcess, 4000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(child.hProcess, 1000);
    }
    DWORD exitCode{};
    const bool gotExit = GetExitCodeProcess(child.hProcess, &exitCode) != FALSE;
    CloseHandle(child.hProcess);
    std::cout << "junction creation: wait=" << wait << " exit=" << exitCode << '\n';
    require(wait == WAIT_OBJECT_0 && gotExit && exitCode == 0, "required directory junction unavailable");

    WIN32_FIND_DATAW data{};
    const auto found = FindFirstFileW(junction.c_str(), &data);
    require(found != INVALID_HANDLE_VALUE, "read junction tag");
    FindClose(found);
    require((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT,
        "expected a directory junction mount-point tag");
    const auto reason = controller_error(options);
    std::cout << "junction tag=0x" << std::hex << data.dwReserved0 << std::dec
        << " controller_reason=" << reason << " dispatch_calls=" << dispatches << '\n';
    cleanup.removed = RemoveDirectoryW(junction.c_str()) != FALSE;
    require(cleanup.removed, "remove owned junction itself");
    return reason == "reparse_or_root_escape path=programs/fg-lite-any-dp4a-srgb8/pack.nrx" && dispatches == 0;
}
