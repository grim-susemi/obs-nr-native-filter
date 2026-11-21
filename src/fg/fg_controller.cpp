// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

namespace nr::fg::vfg {
namespace {
namespace fs = std::filesystem;
struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
    HANDLE release() { auto out = value; value = nullptr; return out; }
};
void require(bool ok, const char* op)
{
    if (!ok) throw ipc::Error(ipc::Status::ProtocolError, HRESULT_FROM_WIN32(GetLastError()), op);
}
std::wstring env_path(const wchar_t* key)
{
    wchar_t value[4096]{};
    const auto n = GetEnvironmentVariableW(key, value, ARRAYSIZE(value));
    if (!n || n >= ARRAYSIZE(value) || !fs::path(value).is_absolute() || wcschr(value, L'"')) return {};
    return value;
}
fs::path own_module_dir()
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&own_module_dir), &mod))
        return {};
    wchar_t buf[4096]{};
    const auto n = GetModuleFileNameW(mod, buf, ARRAYSIZE(buf));
    if (!n || n >= ARRAYSIZE(buf)) return {};
    return fs::path(buf).parent_path();
}
fs::path host_exe_dir()
{
    wchar_t buf[4096]{};
    const auto n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    if (!n || n >= ARRAYSIZE(buf)) return {};
    return fs::path(buf).parent_path();
}
bool has_vfg_dlls(const fs::path& dir)
{
    std::error_code ec;
    return fs::is_regular_file(dir / L"NVVideoEffects.dll", ec) &&
           fs::is_regular_file(dir / L"NVCVImage.dll", ec) &&
           fs::is_regular_file(dir / L"nvVFXVideoFrameGeneration.dll", ec) &&
           fs::is_regular_file(dir / L"nvngxruntime.dll", ec) &&
           fs::is_regular_file(dir / L"cudart64_12.dll", ec);
}
std::wstring resolve_presenter_exe()
{
    if (auto p = env_path(L"OBS_NR_VFG_PRESENTER"); !p.empty()) return p;
    const auto mod = own_module_dir();
    const auto exe = host_exe_dir();
    const std::array<fs::path, 4> candidates{
        mod / L"obs-nr-fg-presenter.exe",
        mod / L".." / L".." / L"bin" / L"64bit" / L"obs-nr-fg-presenter.exe",
        exe / L"obs-nr-fg-presenter.exe",
        exe / L"obs-plugins" / L"64bit" / L"obs-nr-fg-presenter.exe"
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        const auto norm = c.lexically_normal();
        if (!norm.empty() && fs::is_regular_file(norm, ec) && !wcschr(norm.c_str(), L'"')) return norm.wstring();
    }
    throw ipc::Error(ipc::Status::ProtocolError, ERROR_FILE_NOT_FOUND, "obs-nr-fg-presenter.exe not found");
}
std::wstring resolve_log_path()
{
    if (auto p = env_path(L"OBS_NR_VFG_LOG"); !p.empty()) return p;
    wchar_t tmp[4096]{};
    const auto n = GetTempPathW(ARRAYSIZE(tmp), tmp);
    if (!n || n >= ARRAYSIZE(tmp)) throw ipc::Error(ipc::Status::ProtocolError, E_FAIL, "temp dir");
    return (fs::path(tmp) / L"obs-nr-vfg-presenter.log").lexically_normal().wstring();
}
} // namespace

std::wstring Controller::find_runtime_dir()
{
    if (auto p = env_path(L"OBS_NR_VFG_RUNTIME"); !p.empty() && has_vfg_dlls(p)) return p;
    const auto mod = own_module_dir();
    const auto exe = host_exe_dir();
    std::vector<fs::path> candidates{
        exe / L"nvidia-vfg",
        mod / L"nvidia-vfg",
        mod / L".." / L".." / L"bin" / L"64bit" / L"nvidia-vfg"
    };
    wchar_t localApp[4096]{};
    const auto la = GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, ARRAYSIZE(localApp));
    if (la && la < ARRAYSIZE(localApp)) {
        candidates.push_back(fs::path(localApp) / L"Programs" / L"Veyra-2.1.9-NVIDIA-Virtual-Camera" /
            L"Veyra-2.1.9-virtual-camera-NVIDIA-win64-portable" / L"runtime" / L"nvidia-vfg");
    }
    wchar_t progFiles[4096]{};
    const auto pf = GetEnvironmentVariableW(L"ProgramFiles", progFiles, ARRAYSIZE(progFiles));
    if (pf && pf < ARRAYSIZE(progFiles)) {
        candidates.push_back(fs::path(progFiles) / L"Veyra-NRVideo" / L"runtime" / L"nvidia-vfg");
    }
    for (const auto& c : candidates) {
        const auto norm = c.lexically_normal();
        if (!norm.empty() && has_vfg_dlls(norm) && !wcschr(norm.c_str(), L'"')) return norm.wstring();
    }
    return {};
}

Controller::Controller(const LaunchOptions& options) : options_(options)
{
    const auto exe = resolve_presenter_exe();
    const auto log = resolve_log_path();
    const auto runtime = options.multiplier > 1 ? find_runtime_dir() : exe.substr(0, exe.find_last_of(L"\\/"));
    if (options.multiplier > 1 && runtime.empty())
        throw ipc::Error(ipc::Status::ProtocolError, ERROR_FILE_NOT_FOUND, "NVIDIA VFG runtime DLLs not found");
    Handle job{CreateJobObjectW(nullptr, nullptr)}, parent;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(Session), nullptr)};
    require(job.value && mapping.value, "VFG job/session mapping");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "VFG job limits");
    require(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent.value,
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE, TRUE, 0), "VFG parent identity");
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    require(InitializeProcThreadAttributeList(attrs, 1, 0, &bytes), "VFG attribute list");
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Attributes() { DeleteProcThreadAttributeList(p); } } cleanup{attrs};
    HANDLE allowed[]{parent.value, mapping.value};
    require(UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, allowed, sizeof(allowed), nullptr, nullptr), "VFG inheritance list");
    const auto num = [](HANDLE h) { return std::to_wstring(reinterpret_cast<uintptr_t>(h)); };
    const auto mult = std::clamp<uint32_t>(options.multiplier, 1, max_multiplier);
    const auto modeStr = mult == 1 ? L"off" : (std::to_wstring(mult) + L"x");
    auto args = L"\"" + exe + L"\" " + num(parent.value) + L" " + num(mapping.value)
        + L" \"" + runtime + L"\" \"" + log + L"\" " + modeStr;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attrs;
    PROCESS_INFORMATION child{};
    auto* view = static_cast<Session*>(MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, sizeof(Session)));
    require(view != nullptr, "VFG map session");
    struct View { Session* p; ~View() { if (p) UnmapViewOfFile(p); } } guard{view};
    InterlockedExchange(&view->fpsOverlay, static_cast<LONG>(std::min<uint32_t>(options.fpsOverlay, 2)));
    InterlockedExchange(&view->lowLatency, static_cast<LONG>(options.lowLatency ? 1u : 0u));
    InterlockedExchange(&view->gpuPriority, static_cast<LONG>(options.gpuPriority ? 1u : 0u));
    InterlockedExchange(&view->hideFromCapture, static_cast<LONG>(options.hideFromCapture ? 1u : 0u));
    require(CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, runtime.c_str(),
        &startup.StartupInfo, &child), "VFG start presenter");
    Handle process{child.hProcess}, thread{child.hThread};
    if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == DWORD(-1)) {
        const auto error = GetLastError();
        TerminateProcess(process.value, ERROR_CANCELLED);
        SetLastError(error);
        require(false, "VFG bind presenter job");
    }
    AllowSetForegroundWindow(child.dwProcessId);
    view_ = view;
    guard.p = nullptr;
    child_ = process.release();
    mapping_ = mapping.release();
    job_ = job.release();
}

Controller::~Controller()
{
    stop();
    if (job_) CloseHandle(job_);
    if (child_) CloseHandle(child_);
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
}

bool Controller::offer(const ipc::Offer& offer, const ipc::Shape& shape, LUID adapter) noexcept
{
    if (offer.mapping.size() >= ARRAYSIZE(view_->mapping)) return false;
    const auto prior = InterlockedCompareExchange64(&view_->state, 0, 0);
    if (prior != 0 && prior != 3) return false;
    view_->generation = offer.generation;
    view_->width = shape.width;
    view_->height = shape.height;
    view_->splitCompare = shape.splitCompare ? 1u : 0u;
    view_->multiplier = std::clamp<uint32_t>(options_.multiplier, 1, max_multiplier);
    view_->quality = static_cast<uint32_t>(options_.quality);
    view_->adapter = adapter;
    InterlockedExchange(&view_->fpsOverlay, static_cast<LONG>(std::min<uint32_t>(options_.fpsOverlay, 2)));
    InterlockedExchange(&view_->lowLatency, static_cast<LONG>(options_.lowLatency ? 1u : 0u));
    InterlockedExchange(&view_->gpuPriority, static_cast<LONG>(options_.gpuPriority ? 1u : 0u));
    InterlockedExchange(&view_->hideFromCapture, static_cast<LONG>(options_.hideFromCapture ? 1u : 0u));
    wcscpy_s(view_->mapping, offer.mapping.c_str());
    if (!options_.monitorId.empty() && options_.monitorId.size() < ARRAYSIZE(view_->monitorId)) {
        wcscpy_s(view_->monitorId, options_.monitorId.c_str());
    } else {
        view_->monitorId[0] = L'\0';
    }
    return InterlockedCompareExchange64(&view_->state, 1, prior) == prior;
}
} // namespace nr::fg::vfg
