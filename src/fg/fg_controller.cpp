// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include "anr_dll_validator.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace nr::fg::vfg {
namespace {
namespace fs = std::filesystem;
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE h) noexcept : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.release()) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }
    void reset(HANDLE h = nullptr) noexcept
    {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = h;
    }
    HANDLE release() noexcept { return std::exchange(value, nullptr); }
};
struct ModulePin {
    HMODULE mod{};
    ModulePin() = default;
    ~ModulePin() { reset(); }
    ModulePin(const ModulePin&) = delete;
    ModulePin& operator=(const ModulePin&) = delete;
    ModulePin(ModulePin&& other) noexcept : mod(other.release()) {}
    ModulePin& operator=(ModulePin&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }
    bool pin(LPCVOID addr) noexcept
    {
        reset();
        return GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            static_cast<LPCWSTR>(addr),
            &mod) != FALSE && mod != nullptr;
    }
    void reset(HMODULE next = nullptr) noexcept
    {
        if (mod) FreeLibrary(mod);
        mod = next;
    }
    HMODULE release() noexcept { return std::exchange(mod, nullptr); }
};
void require(bool ok, const char* op)
{
    if (!ok) throw ipc::Error(ipc::Status::ProtocolError, HRESULT_FROM_WIN32(GetLastError()), op);
}
// Exhaustive backend policy helpers: a new Backend enumerator is a compile error
// here instead of silently inheriting a negated check somewhere else.
constexpr bool backend_is_pushflow(Backend backend) noexcept
{
    switch (backend) {
    case Backend::NativeVfg:
    case Backend::AnrExperimental:
    case Backend::XeFg:
    case Backend::XeMfg:
        return false;
    case Backend::PushFlow:
        return true;
    }
    return false;
}
constexpr bool backend_is_anr(Backend backend) noexcept
{
    switch (backend) {
    case Backend::NativeVfg:
    case Backend::PushFlow:
    case Backend::XeFg:
    case Backend::XeMfg:
        return false;
    case Backend::AnrExperimental:
        return true;
    }
    return false;
}
constexpr bool backend_uses_native_vfg(Backend backend) noexcept
{
    switch (backend) {
    case Backend::NativeVfg:
        return true;
    case Backend::PushFlow:
    case Backend::AnrExperimental:
    case Backend::XeFg:
    case Backend::XeMfg:
        return false;
    }
    return false;
}
constexpr bool backend_supports_auto_fallback(Backend backend) noexcept
{
    switch (backend) {
    case Backend::NativeVfg:
        return false;
    case Backend::PushFlow:
    case Backend::AnrExperimental:
    case Backend::XeFg:
    case Backend::XeMfg:
        return true;
    }
    return false;
}
// A degraded 1x session must not carry the failed backend's runtime selection
// into the recovery child: multiplier 1 is accepted with those fields cleared.
void clear_backend_fields(LaunchOptions& options) noexcept
{
    switch (options.backend) {
    case Backend::NativeVfg:
        break;
    case Backend::PushFlow:
        options.pushflowRuntimeDir.clear();
        break;
    case Backend::AnrExperimental:
        options.anrDllPath.clear();
        options.experimentalAnrOptIn = 0;
        break;
    case Backend::XeFg:
        options.xessRuntimeDir.clear();
        break;
    case Backend::XeMfg:
        options.xessRuntimeDir.clear();
        options.xemfgDllPath.clear();
        options.experimentalXeMfgOptIn = 0;
        break;
    }
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
bool has_pushflow_runtime(const fs::path& dir, PushFlowTier tier)
{
    const auto model = tier == PushFlowTier::Quality ? L"pushflow-quality.onnx" : L"pushflow-fast.onnx";
    std::error_code ec;
    return dir.is_absolute() && fs::is_regular_file(dir / L"onnxruntime.dll", ec) &&
        fs::is_regular_file(dir / L"DirectML.dll", ec) && fs::is_regular_file(dir / L"models" / model, ec);
}
using VersionTriple = std::array<uint32_t, 3>;
std::optional<VersionTriple> parse_version_dir(std::wstring_view name) noexcept
{
    VersionTriple out{};
    size_t i = 0;
    for (size_t part = 0; part < out.size(); ++part) {
        const auto begin = i;
        uint64_t value = 0;
        while (i < name.size() && name[i] >= L'0' && name[i] <= L'9') {
            if (i - begin >= 9) return std::nullopt;
            value = value * 10 + static_cast<uint64_t>(name[i] - L'0');
            ++i;
        }
        if (i == begin) return std::nullopt;
        out[part] = static_cast<uint32_t>(value);
        if (part + 1 < out.size() && (i >= name.size() || name[i++] != L'.')) return std::nullopt;
    }
    return i == name.size() ? std::optional<VersionTriple>(out) : std::nullopt;
}
// Veyra installs versioned runtimes under LOCALAPPDATA; compare parsed versions numerically so 2.10.0 beats 2.2.1.
fs::path newest_veyra_runtime(const std::wstring& localApp)
{
    const fs::path versions = fs::path(localApp) / L"Programs" / L"Veyra (NVIDIA)" / L"versions";
    std::error_code ec;
    std::optional<VersionTriple> bestVersion;
    fs::path best;
    for (fs::directory_iterator it(versions, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code typeEc;
        if (!it->is_directory(typeEc) || typeEc) continue;
        const auto version = parse_version_dir(it->path().filename().wstring());
        if (!version) continue;
        const auto runtime = (it->path() / L"runtime" / L"nvidia-vfg").lexically_normal();
        if (!has_vfg_dlls(runtime)) continue;
        if (!bestVersion || *version > *bestVersion) {
            bestVersion = version;
            best = runtime;
        }
    }
    return best;
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
std::wstring default_log_path_candidate()
{
    wchar_t tmp[4096]{};
    const auto n = GetTempPathW(ARRAYSIZE(tmp), tmp);
    if (!n || n >= ARRAYSIZE(tmp)) throw ipc::Error(ipc::Status::ProtocolError, E_FAIL, "temp dir");
    return (fs::path(tmp) / L"obs-nr-vfg-presenter.log").lexically_normal().wstring();
}
fs::path marker_dir()
{
    const auto local = env_path(L"LOCALAPPDATA");
    return local.empty() ? fs::path{} : fs::path(local) / L"obs-nr-native-filter";
}
constexpr char kUserClosedFallbackPayload[] = "reason=user_closed_marker_cleanup_failed\n";

std::string format_marker_token_a(uint64_t token)
{
    char buf[24]{};
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(token));
    return buf;
}
std::optional<uint64_t> parse_token_from_filename(std::wstring_view name) noexcept
{
    constexpr std::wstring_view prefix = L"pushflow-starting-";
    constexpr std::wstring_view suffix = L".txt";
    if (name.size() != prefix.size() + 16 + suffix.size() ||
        !name.starts_with(prefix) || !name.ends_with(suffix)) return std::nullopt;
    const auto hex = name.substr(prefix.size(), 16);
    uint64_t out = 0;
    for (wchar_t ch : hex) {
        out <<= 4;
        if (ch >= L'0' && ch <= L'9') out |= static_cast<uint64_t>(ch - L'0');
        else if (ch >= L'a' && ch <= L'f') out |= static_cast<uint64_t>(ch - L'a' + 10);
        else if (ch >= L'A' && ch <= L'F') out |= static_cast<uint64_t>(ch - L'A' + 10);
        else return std::nullopt;
    }
    return out ? std::optional<uint64_t>(out) : std::nullopt;
}
std::atomic<detail::MarkerMutexCreateFn> g_markerMutexCreateForTest{nullptr};
HANDLE create_marker_mutex_checked(LPCWSTR name) noexcept
{
    if (const auto fn = g_markerMutexCreateForTest.load(std::memory_order_acquire)) {
        return fn(nullptr, FALSE, name);
    }
    wchar_t failMode[8]{};
    const auto len = GetEnvironmentVariableW(L"OBS_NR_TEST_MARKER_MUTEX_FAIL", failMode, ARRAYSIZE(failMode));
    if (len == 1 && failMode[0] == L'1') {
        SetLastError(ERROR_NO_SYSTEM_RESOURCES);
        return nullptr;
    }
    if (len == 1 && failMode[0] == L'2') {
        SetLastError(ERROR_ALREADY_EXISTS);
        return CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    return CreateMutexW(nullptr, FALSE, name);
}
uint64_t allocate_marker_token(Handle& outLock)
{
    constexpr int maxCollisionAttempts = 64;
    static std::atomic_uint64_t seq{1};
    const uint64_t pidMix = static_cast<uint64_t>(GetCurrentProcessId()) << 32;
    for (int attempt = 0; attempt < maxCollisionAttempts; ++attempt) {
        const uint64_t n = seq.fetch_add(1, std::memory_order_relaxed);
        const uint64_t tok = pidMix ^ n ^ (clock_ns() & 0xFFFFFFFFULL);
        if (tok == 0) continue;
        const auto name = marker_mutex_name(tok);
        Handle h{create_marker_mutex_checked(name.c_str())};
        if (!h.value) break; // Non-collision creation failure must not spin or retry.
        if (GetLastError() == ERROR_ALREADY_EXISTS) continue; // Bounded retry ONLY on confirmed name collision.
        outLock = std::move(h);
        return tok;
    }
    return 0;
}
bool is_marker_token_active(uint64_t token) noexcept
{
    if (!token) return false;
    try {
        const auto name = marker_mutex_name(token);
        Handle h{OpenMutexW(SYNCHRONIZE, FALSE, name.c_str())};
        if (h.value) return true;
        // Only treat confirmed absence as dead; preserve other hosts' markers fail-closed on lookup errors.
        return GetLastError() != ERROR_FILE_NOT_FOUND;
    } catch (...) {
        return true;
    }
}

struct PendingIoWrite {
    Handle file;
    Handle event;
    OVERLAPPED ov{};
    std::string text;
    ModulePin waitMod;
    Handle reapedEvent;
    ~PendingIoWrite()
    {
        file.reset();
        event.reset();
        waitMod.reset();
    }
};

struct WriteJobResult {
    std::mutex mtx;
    detail::WriteEventStatus status{};
    Handle doneEvent;
};

struct WriteWorkContext {
    std::wstring path;
    DWORD timeoutMs{};
    bool skipCancelIoEx{};
    ModulePin workMod;
    Handle stallBeforeIoEvent;
    PTP_WAIT tpWait{};
    std::unique_ptr<PendingIoWrite> pending;
    std::shared_ptr<WriteJobResult> result;
    ~WriteWorkContext()
    {
        if (tpWait) {
            CloseThreadpoolWait(tpWait);
            tpWait = nullptr;
        }
    }
};

VOID CALLBACK wait_reap_callback(PTP_CALLBACK_INSTANCE instance, PVOID ctx, PTP_WAIT wait, TP_WAIT_RESULT) noexcept
{
    std::unique_ptr<PendingIoWrite> own(static_cast<PendingIoWrite*>(ctx));
    CloseThreadpoolWait(wait);
    Handle notify = std::move(own->reapedEvent);
    const HMODULE mod = own->waitMod.release();
    own.reset();
    if (notify.value) SetEvent(notify.value);
    notify.reset();
    if (mod) FreeLibraryWhenCallbackReturns(instance, mod);
}

VOID CALLBACK write_work_callback(PTP_CALLBACK_INSTANCE instance, PVOID ctx, PTP_WORK work) noexcept
{
    std::unique_ptr<WriteWorkContext> wctx(static_cast<WriteWorkContext*>(ctx));
    CloseThreadpoolWork(work);
    ModulePin workPin = std::move(wctx->workMod);
    detail::WriteEventStatus localStatus{};
    bool deferred = false;
    try {
        if (wctx->stallBeforeIoEvent.value) {
            WaitForSingleObject(wctx->stallBeforeIoEvent.value, INFINITE);
        }
        auto& p = wctx->pending;
        const bool isPipe = wctx->path.rfind(LR"(\\.\pipe\)", 0) == 0;
        p->file.reset(CreateFileW(wctx->path.c_str(),
            isPipe ? GENERIC_WRITE : FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            isPipe ? OPEN_EXISTING : OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr));
        if (p->file.value == INVALID_HANDLE_VALUE) {
            p->file.release();
        } else {
            localStatus.opened = true;
            p->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (p->event.value) {
                p->ov.hEvent = p->event.value;
                if (!isPipe) {
                    p->ov.Offset = 0xFFFFFFFFu;
                    p->ov.OffsetHigh = 0xFFFFFFFFu;
                }
                DWORD written{};
                if (!WriteFile(p->file.value, p->text.data(),
                        static_cast<DWORD>(p->text.size()), &written, &p->ov)) {
                    if (GetLastError() == ERROR_IO_PENDING) {
                        localStatus.pended = true;
                        if (WaitForSingleObject(p->event.value, wctx->timeoutMs) != WAIT_OBJECT_0) {
                            if (!wctx->skipCancelIoEx) {
                                localStatus.cancelled = (CancelIoEx(p->file.value, &p->ov) != FALSE);
                            }
                            if (WaitForSingleObject(p->event.value, 50) == WAIT_OBJECT_0) {
                                DWORD tr{};
                                if (!GetOverlappedResult(p->file.value, &p->ov, &tr, FALSE) &&
                                    GetLastError() == ERROR_OPERATION_ABORTED) {
                                    localStatus.aborted = true;
                                }
                            } else {
                                // Kernel I/O is still in flight; transfer ownership to pre-allocated TP_WAIT.
                                localStatus.deferredToPool = true;
                                deferred = true;
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {
    }

    if (deferred) {
        {
            std::lock_guard lock(wctx->result->mtx);
            wctx->result->status = localStatus;
        }
        const HANDLE ev = wctx->pending->event.value;
        PTP_WAIT wait = wctx->tpWait;
        wctx->tpWait = nullptr;
        wctx->pending.release();
        SetEvent(wctx->result->doneEvent.value);
        wctx->result.reset();
        SetThreadpoolWait(wait, ev, nullptr);
        wctx.reset();
        const HMODULE workMod = workPin.release();
        if (workMod) FreeLibraryWhenCallbackReturns(instance, workMod);
        return;
    }

    {
        std::lock_guard lock(wctx->result->mtx);
        wctx->result->status = localStatus;
    }
    if (wctx->tpWait) {
        CloseThreadpoolWait(wctx->tpWait);
        wctx->tpWait = nullptr;
    }
    Handle reapNotify = std::move(wctx->pending->reapedEvent);
    wctx->pending.reset();
    SetEvent(wctx->result->doneEvent.value);
    if (reapNotify.value) SetEvent(reapNotify.value);
    reapNotify.reset();
    wctx->result.reset();
    wctx.reset();
    const HMODULE workMod = workPin.release();
    if (workMod) FreeLibraryWhenCallbackReturns(instance, workMod);
}

void write_event(const char* event) noexcept
{
    try {
        detail::write_event_bounded(detail::resolve_log_path(), event ? event : "");
    } catch (...) {
    }
}
bool clear_starting_marker(const fs::path& marks, uint64_t ownerToken, std::error_code& ec) noexcept
{
    ec.clear();
    if (marks.empty()) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }
    try {
        fs::remove(marks / starting_marker_filename(ownerToken), ec);
        return !ec;
    } catch (...) {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return false;
    }
}
bool read_bounded_marker_text(const fs::path& file, std::string& out) noexcept
{
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        char buf[129]{};
        in.read(buf, 128);
        const auto got = in.gcount();
        if (in.bad() || (!in.eof() && in.peek() != std::char_traits<char>::eof())) return false;
        out.assign(buf, static_cast<size_t>(got));
        return true;
    } catch (...) {
        return false;
    }
}
struct ParsedUserClosedFallback {
    bool valid{};
    bool hasToken{};
    uint64_t token{};
};
ParsedUserClosedFallback parse_user_closed_fallback(const fs::path& userClosedMark) noexcept
{
    std::string content;
    if (!read_bounded_marker_text(userClosedMark, content)) return {};
    if (content == kUserClosedFallbackPayload || content == "reason=user_closed_marker_cleanup_failed\r\n") {
        return {true, false, 0};
    }
    constexpr std::string_view prefix = "token=";
    if (!content.starts_with(prefix)) return {};
    std::string_view sv(content);
    sv.remove_prefix(prefix.size());
    if (sv.ends_with("\r\n")) sv.remove_suffix(2);
    else if (sv.ends_with("\n")) sv.remove_suffix(1);
    if (sv.empty() || sv.size() > 16) return {};
    uint64_t tok = 0;
    const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), tok, 16);
    if (res.ec != std::errc{} || res.ptr != sv.data() + sv.size() || tok == 0) return {};
    return {true, true, tok};
}
enum class MarkerCleanupStatus { NotNeeded, Pending, Succeeded, Failed };
} // namespace

namespace detail {
WriteEventStatus write_event_bounded(const std::wstring& path, const std::string& text,
    DWORD timeoutMs, const WriteTestHooks* hooks) noexcept
{
    WriteEventStatus status{};
    try {
        if (path.empty() || text.empty()) return status;
        if (hooks && hooks->failPoolPrep) return status;

        ModulePin workPin, waitPin;
        if (!workPin.pin(&write_event_bounded) || !waitPin.pin(&write_event_bounded)) {
            return status;
        }

        auto result = std::make_shared<WriteJobResult>();
        result->doneEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!result->doneEvent.value) return status;

        auto pending = std::make_unique<PendingIoWrite>();
        pending->waitMod = std::move(waitPin);
        pending->text = text;
        if (hooks && hooks->reapedEvent) {
            HANDLE dupReap = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), hooks->reapedEvent,
                    GetCurrentProcess(), &dupReap, EVENT_MODIFY_STATE, FALSE, 0)) {
                pending->reapedEvent.reset(dupReap);
            }
        } else {
            wchar_t reapName[256]{};
            if (GetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_REAPED_EVENT", reapName, ARRAYSIZE(reapName))) {
                pending->reapedEvent.reset(OpenEventW(EVENT_MODIFY_STATE, FALSE, reapName));
            }
        }

        auto wctx = std::make_unique<WriteWorkContext>();
        wctx->path = path;
        wctx->timeoutMs = timeoutMs;
        wctx->skipCancelIoEx = hooks && hooks->skipCancelIoEx;
        wctx->workMod = std::move(workPin);
        if (hooks && hooks->stallBeforeIoEvent) {
            HANDLE dupStall = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), hooks->stallBeforeIoEvent,
                    GetCurrentProcess(), &dupStall, SYNCHRONIZE, FALSE, 0)) {
                wctx->stallBeforeIoEvent.reset(dupStall);
            }
        } else {
            wchar_t evName[256]{};
            if (GetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_STALL_EVENT", evName, ARRAYSIZE(evName))) {
                wctx->stallBeforeIoEvent.reset(OpenEventW(SYNCHRONIZE, FALSE, evName));
            }
        }
        wctx->result = result;
        wctx->pending = std::move(pending);

        wctx->tpWait = CreateThreadpoolWait(wait_reap_callback, wctx->pending.get(), nullptr);
        if (!wctx->tpWait) return status;

        PTP_WORK tpWork = CreateThreadpoolWork(write_work_callback, wctx.get(), nullptr);
        if (!tpWork) return status;

        wctx.release();
        SubmitThreadpoolWork(tpWork);

        const DWORD waitBudget = timeoutMs + 150;
        if (WaitForSingleObject(result->doneEvent.value, waitBudget) == WAIT_OBJECT_0) {
            std::lock_guard lock(result->mtx);
            return result->status;
        }
        status.timedOut = true;
        return status;
    } catch (...) {
    }
    return status;
}
std::wstring resolve_log_path()
{
    if (auto p = env_path(L"OBS_NR_VFG_LOG"); !p.empty()) return p;
    const auto path = fs::path(default_log_path_candidate());
    static std::once_flag resetOnce;
    std::call_once(resetOnce, [&path] {
        // A previous real-usage run may have left the unbounded %TEMP% log behind.
        std::error_code ec;
        fs::resize_file(path, 0, ec);
    });
    return path.wstring();
}
void set_marker_mutex_create_fn_for_test(MarkerMutexCreateFn fn) noexcept
{
    g_markerMutexCreateForTest.store(fn, std::memory_order_release);
}
} // namespace detail

struct Controller::ChildSession {
    HANDLE job{}, child{}, mapping{};
    Session* view{};
    HANDLE exitSettledEvent{};
    std::wstring logPath;
    uint64_t markerOwnerToken{};
    Handle markerTokenLock;
    std::atomic<MarkerCleanupStatus> markerCleanupStatus{MarkerCleanupStatus::NotNeeded};
    std::jthread observer;

    ~ChildSession()
    {
        // Terminate any live child in the kill-on-close job BEFORE joining observer.
        if (job) {
            CloseHandle(job);
            job = nullptr;
        }
        if (observer.joinable()) observer.join();
        markerTokenLock.reset();
        if (exitSettledEvent) CloseHandle(exitSettledEvent);
        if (child) CloseHandle(child);
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
    }
};

std::wstring Controller::find_runtime_dir()
{
    if (auto p = env_path(L"OBS_NR_VFG_RUNTIME"); !p.empty() && has_vfg_dlls(p)) return p;
    wchar_t localApp[4096]{};
    const auto la = GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, ARRAYSIZE(localApp));
    const bool hasLocalApp = la && la < ARRAYSIZE(localApp);
    const auto mod = own_module_dir();
    const auto exe = host_exe_dir();
    std::vector<fs::path> candidates{
        exe / L"nvidia-vfg",
        mod / L"nvidia-vfg",
        mod / L".." / L".." / L"bin" / L"64bit" / L"nvidia-vfg"
    };
    if (hasLocalApp) {
        if (auto versioned = newest_veyra_runtime(localApp); !versioned.empty()) candidates.push_back(versioned);
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

std::wstring Controller::find_pushflow_dir(PushFlowTier tier)
{
    const std::array<fs::path, 4> candidates{env_path(L"OBS_NR_PUSHFLOW_RUNTIME"),
        host_exe_dir() / L"pushflow", own_module_dir() / L"pushflow",
        own_module_dir() / L".." / L".." / L"bin" / L"64bit" / L"pushflow"};
    for (const auto& candidate : candidates) {
        const auto dir = candidate.lexically_normal();
        if (has_pushflow_runtime(dir, tier)) return dir.wstring();
    }
    return {};
}

Controller::Controller(const LaunchOptions& options) : Controller(options, false) {}

Controller::Controller(const LaunchOptions& options, bool fallback) : options_(options)
{
    Session header{};
    header.backend = options.backend;
    header.pushflowTier = options.pushflowTier;
    header.gpuShotChange = options.gpuShotChange != 0;
    header.experimentalAnrOptIn = options.experimentalAnrOptIn;
    header.dedupMode = static_cast<LONG>(options.dedupMode);
    header.liveAnchor = static_cast<LONG>(options.liveAnchor);
    header.transportIntervalNs = options.transportIntervalNs;
    header.launchEpoch = fallback ? 2 : 1;
    header.experimentalXeMfgOptIn = options.experimentalXeMfgOptIn;
    header.xefgStabilityMask = options.xefgStabilityMask;
    header.sharedNvofEnabled = options.sharedNvofEnabled;
    require(options.pushflowRuntimeDir.size() < ARRAYSIZE(header.pushflowRuntimeDir) &&
        options.anrDllPath.size() < ARRAYSIZE(header.anrDllPath) &&
        options.xessRuntimeDir.size() < ARRAYSIZE(header.xessRuntimeDir) &&
        options.xemfgDllPath.size() < ARRAYSIZE(header.xemfgDllPath), "VFG runtime path too long");
    std::copy(options.anrDllPath.begin(), options.anrDllPath.end(), header.anrDllPath);
    std::copy(options.xessRuntimeDir.begin(), options.xessRuntimeDir.end(), header.xessRuntimeDir);
    std::copy(options.xemfgDllPath.begin(), options.xemfgDllPath.end(), header.xemfgDllPath);
    // Stamp the requested multiplier before admission so a 1x recovery header is
    // validated under its real mode rather than the zero-initialised default.
    header.multiplier = std::clamp<uint32_t>(options.multiplier, 1, max_multiplier);
    require(validate_session_header(header), "VFG invalid session header");
    std::unique_ptr<anr::PinnedAddon> anrPin;
    if (backend_is_anr(options.backend) && !fallback) {
        auto result = anr::validate_addon(options.anrDllPath, options.experimentalAnrOptIn == 1);
        std::string reason = anr::status_name(result.status);
        switch (result.status) {
        case anr::ValidationStatus::Accepted:
            anrPin = std::move(result.addon);
            require(anrPin->canonicalPath.size() < ARRAYSIZE(header.anrDllPath), "VFG canonical ANR path too long");
            std::ranges::fill(header.anrDllPath, L'\0');
            std::ranges::copy(anrPin->canonicalPath, header.anrDllPath);
            options_.anrDllPath = anrPin->canonicalPath;
            write_event((std::string("event=anr_validated sha256=") + anrPin->sha256 +
                (anrPin->sha256 == anr::b15Sha256 ? "\n" : " warning=b15_sha256_mismatch\n")).c_str());
            break;
        case anr::ValidationStatus::IncompleteAddonMissingCompanions:
            options_.multiplier = 1;
            write_event(("event=anr_validation_fallback reason=" + reason + " missing=" +
                std::string(result.relativePath) + "\n").c_str());
            break;
        case anr::ValidationStatus::ReparseOrRootEscape:
        case anr::ValidationStatus::IoFailure:
            if (!result.relativePath.empty()) reason += " path=" + std::string(result.relativePath);
            [[fallthrough]];
        case anr::ValidationStatus::OptInRequired:
        case anr::ValidationStatus::InvalidPath:
        case anr::ValidationStatus::InvalidSize:
        case anr::ValidationStatus::InvalidPe:
        case anr::ValidationStatus::MissingExport:
        case anr::ValidationStatus::InvalidExport:
        case anr::ValidationStatus::HashFailure:
            throw ipc::Error(ipc::Status::ProtocolError, E_ACCESSDENIED, reason.c_str());
        }
    }
    const auto exe = resolve_presenter_exe();
    const auto log = detail::resolve_log_path();
    const auto exeDir = fs::path(exe).parent_path().wstring();
    const bool nativeFg = backend_uses_native_vfg(options.backend) && options.multiplier > 1;
    const auto runtime = nativeFg ? find_runtime_dir() : exeDir;
    if (nativeFg && runtime.empty())
        throw ipc::Error(ipc::Status::ProtocolError, ERROR_FILE_NOT_FOUND, "NVIDIA VFG runtime DLLs not found");
    if (backend_is_pushflow(options.backend) && options.multiplier > 1 && !fallback) {
        const auto marks = marker_dir();
        const auto legacyStarting = marks / L"pushflow-starting.txt";
        const auto quarantine = marks / L"pushflow-crash-quarantine.txt";
        const auto userClosedMark = marks / L"pushflow-user-closed.txt";
        std::error_code ec;
        bool fallbackFailed = false;
        const bool userClosedExists = !marks.empty() && fs::exists(userClosedMark, ec);
        if (ec) {
            fallbackFailed = true;
            write_event("event=pushflow_fallback_probe_failed\n");
        } else if (userClosedExists) {
            const auto parsed = parse_user_closed_fallback(userClosedMark);
            std::error_code qEc;
            const bool hasQuarantine = fs::exists(quarantine, qEc);
            if (qEc || !parsed.valid) {
                fallbackFailed = true;
                write_event(!parsed.valid ? "event=pushflow_fallback_corrupt\n" : "event=pushflow_quarantine_probe_failed\n");
                std::error_code rmEc;
                fs::remove(userClosedMark, rmEc);
            } else if (!hasQuarantine) {
                std::error_code clearEc;
                if (clear_starting_marker(marks, parsed.hasToken ? parsed.token : 0, clearEc)) {
                    std::error_code rmEc;
                    if (!fs::remove(userClosedMark, rmEc) && rmEc) {
                        fallbackFailed = true;
                        write_event("event=pushflow_fallback_remove_failed\n");
                    }
                } else {
                    fallbackFailed = true;
                    write_event("event=pushflow_marker_cleanup_failed\n");
                }
            } else {
                std::error_code rmEc;
                if (!fs::remove(userClosedMark, rmEc) && rmEc) {
                    fallbackFailed = true;
                    write_event("event=pushflow_fallback_remove_failed\n");
                }
            }
        }
        std::error_code scanEc, qCheckEc;
        bool deadStartingFound = false;
        std::vector<fs::path> deadMarkers;
        if (!marks.empty()) {
            std::error_code dirEc;
            const bool marksDirExists = fs::exists(marks, dirEc);
            if (dirEc) {
                scanEc = dirEc;
            } else if (marksDirExists) {
                if (fs::exists(legacyStarting, scanEc)) {
                    deadStartingFound = true;
                    deadMarkers.push_back(legacyStarting);
                }
                if (!scanEc) {
                    for (fs::directory_iterator it(marks, scanEc), end; !scanEc && it != end; it.increment(scanEc)) {
                        const auto fname = it->path().filename().wstring();
                        if (const auto tok = parse_token_from_filename(fname)) {
                            if (!is_marker_token_active(*tok)) {
                                deadStartingFound = true;
                                deadMarkers.push_back(it->path());
                            }
                        }
                    }
                }
            }
        }
        const bool qExists = !marks.empty() && fs::exists(quarantine, qCheckEc);
        if (fallbackFailed || scanEc || qCheckEc || deadStartingFound || qExists) {
            write_event("event=pushflow_previous_crash_detected\n");
            if (!marks.empty()) {
                std::error_code rmEc;
                for (const auto& m : deadMarkers) fs::remove(m, rmEc);
                fs::remove(quarantine, rmEc);
            }
            options_.multiplier = 1;
        } else {
            const auto dir = has_pushflow_runtime(options.pushflowRuntimeDir, options.pushflowTier)
                ? options.pushflowRuntimeDir : find_pushflow_dir(options.pushflowTier);
            require(dir.size() < ARRAYSIZE(header.pushflowRuntimeDir), "PushFlow runtime path too long");
            std::copy(dir.begin(), dir.end(), header.pushflowRuntimeDir);
        }
    }
    Handle markerLock;
    if (backend_is_pushflow(options_.backend) && options_.multiplier > 1) {
        header.markerOwnerToken = allocate_marker_token(markerLock);
        if (header.markerOwnerToken == 0) {
            write_event("event=pushflow_marker_lock_failed_fallback_1x\n");
            options_.multiplier = 1;
            options_.pushflowRuntimeDir.clear();
            std::fill(std::begin(header.pushflowRuntimeDir), std::end(header.pushflowRuntimeDir), L'\0');
        }
    }
    require(validate_session_header(header), "VFG invalid session header");
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
    const auto mult = std::clamp<uint32_t>(options_.multiplier, 1, max_multiplier);
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
    *view = header;
    view->multiplier = mult;
    require(validate_session_header(*view), "VFG invalid mapped session header");
    InterlockedExchange(&view->fpsOverlay, static_cast<LONG>(std::min<uint32_t>(options.fpsOverlay, 2)));
    InterlockedExchange(&view->lowLatency, static_cast<LONG>(options.lowLatency ? 1u : 0u));
    InterlockedExchange(&view->gpuPriority, static_cast<LONG>(options.gpuPriority ? 1u : 0u));
    InterlockedExchange(&view->hideFromCapture, static_cast<LONG>(options.hideFromCapture ? 1u : 0u));
    view->hudLock = options.hudLock != 0;
    view->pacingRepair = options.pacingRepair != 0;
    view->adaptiveFg = options.adaptiveFg != 0;
    view->adaptiveTargetFps = options.adaptiveTargetFps <= LONG_MAX ? static_cast<LONG>(options.adaptiveTargetFps) : 0;
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
    auto session = std::make_unique<ChildSession>();
    Handle exitSettled{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(exitSettled.value != nullptr, "VFG exit settled event");
    session->view = view;
    guard.p = nullptr;
    session->child = process.release();
    session->mapping = mapping.release();
    session->job = job.release();
    session->exitSettledEvent = exitSettled.release();
    session->logPath = log;
    session->markerOwnerToken = header.markerOwnerToken;
    session->markerTokenLock = std::move(markerLock);
    session->markerCleanupStatus.store(
        session->markerOwnerToken != 0 ? MarkerCleanupStatus::Pending : MarkerCleanupStatus::NotNeeded,
        std::memory_order_release);
    session->observer = std::jthread([s = session.get(), backend = options_.backend, marks = marker_dir()] {
        WaitForSingleObject(s->child, INFINITE);
        const auto reason = read_exit_reason(*s->view);
        const auto wdMs = InterlockedCompareExchange64(&s->view->watchdogElapsedMs, 0, 0);
        const auto wdInit = InterlockedCompareExchange(&s->view->watchdogInitializing, 0, 0) != 0;
        const auto forced = InterlockedCompareExchange(&s->view->forceTerminated, 0, 0) != 0;
        const auto drainFailed = InterlockedCompareExchange(&s->view->drainFailed, 0, 0) != 0;
        bool cleanupFailed = false;
        bool fallbackRecorded = false;
        bool fallbackWriteFailed = false;
        const uint64_t ownerTok = s->markerOwnerToken;
        const bool needsUserCloseCleanup =
            s->markerCleanupStatus.load(std::memory_order_acquire) == MarkerCleanupStatus::Pending && ownerTok != 0;
        if (reason == ExitReason::UserClosed && backend_is_pushflow(backend) && needsUserCloseCleanup) {
            std::error_code ec;
            if (clear_starting_marker(marks, ownerTok, ec)) {
                s->markerCleanupStatus.store(MarkerCleanupStatus::Succeeded, std::memory_order_release);
            } else {
                cleanupFailed = true;
                s->markerCleanupStatus.store(MarkerCleanupStatus::Failed, std::memory_order_release);
                std::error_code dirEc;
                if (!marks.empty() && (fs::create_directories(marks, dirEc) || !dirEc)) {
                    std::ofstream fallbackFile(marks / L"pushflow-user-closed.txt", std::ios::binary | std::ios::trunc);
                    fallbackFile << "token=" << format_marker_token_a(ownerTok) << '\n';
                    fallbackFile.flush();
                    const bool wrote = fallbackFile.good();
                    fallbackFile.close();
                    if (wrote && !fallbackFile.fail()) fallbackRecorded = true;
                    else fallbackWriteFailed = true;
                } else {
                    fallbackWriteFailed = true;
                }
            }
        }
        std::string diag;
        if (drainFailed) diag += "event=error message=presenter_drain_failed\n";
        if (wdMs > 0) {
            char line[128];
            const int n = snprintf(line, sizeof(line),
                "event=watchdog_timeout initializing=%d elapsed_ms=%lld\n",
                wdInit ? 1 : 0, static_cast<long long>(wdMs));
            if (n > 0) diag.append(line, static_cast<size_t>(n));
        }
        if (forced) {
            char line[128];
            const int n = snprintf(line, sizeof(line),
                "event=shutdown_force_terminate watchdog=%d exit_reason=%ld\n",
                reason == ExitReason::Watchdog, static_cast<LONG>(reason));
            if (n > 0) diag.append(line, static_cast<size_t>(n));
        }
        if (cleanupFailed) diag += "event=pushflow_marker_cleanup_failed\n";
        if (fallbackRecorded) diag += "event=pushflow_user_closed_fallback_recorded\n";
        if (fallbackWriteFailed) diag += "event=pushflow_fallback_write_failed\n";
        s->markerTokenLock.reset();
        if (!diag.empty()) detail::write_event_bounded(s->logPath, diag, 150);
        SetEvent(s->exitSettledEvent);
    });
    session_ = std::move(session);
    Handle relaunchSettled{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(relaunchSettled.value != nullptr, "VFG relaunch settled event");
    // Start relaunchWorker_ last, after session_ is committed, so constructor errors never deadlock unwind.
    if (!fallback && backend_supports_auto_fallback(options.backend)) {
        auto recovery = options_;
        recovery.multiplier = 1;
        clear_backend_fields(recovery);
        relaunchWorker_ = std::jthread([this, recovery = std::move(recovery)] { run_fallback(recovery); });
    }
    relaunchSettledEvent_ = relaunchSettled.release();
}

ipc::Peer Controller::peer() const { return ipc::identity(session_->child); }
bool Controller::alive() const noexcept { return session_ && WaitForSingleObject(session_->child, 0) == WAIT_TIMEOUT; }
bool Controller::exited() const noexcept
{
    return relaunchState_.load() != RelaunchState::InFlight &&
        relaunchState_.load() != RelaunchState::Ready &&
        session_ && WaitForSingleObject(session_->child, 0) == WAIT_OBJECT_0;
}
bool Controller::await_exit_cleanup(DWORD timeoutMs) const noexcept
{
    return session_ && session_->exitSettledEvent &&
        WaitForSingleObject(session_->exitSettledEvent, timeoutMs) == WAIT_OBJECT_0;
}
bool Controller::user_close_marker_cleaned() const noexcept
{
    if (!session_) return false;
    const auto status = session_->markerCleanupStatus.load(std::memory_order_acquire);
    return status == MarkerCleanupStatus::Succeeded || status == MarkerCleanupStatus::NotNeeded;
}
bool Controller::log_write_in_flight() const noexcept
{
    return session_ && session_->view &&
        InterlockedCompareExchange(&session_->view->logWriteInFlight, 0, 0) > 0;
}
bool Controller::request_resize() noexcept
{
    return session_ && session_->view && InterlockedCompareExchange64(&session_->view->state, 2, 1) == 1;
}
bool Controller::resize_drained() const noexcept
{
    return session_ && session_->view && InterlockedCompareExchange64(&session_->view->state, 3, 3) == 3;
}
ExitReason Controller::exit_reason() const noexcept
{
    return session_ && session_->view ? read_exit_reason(*session_->view) : ExitReason::None;
}

void Controller::request_stop(bool userStop) noexcept
{
    stopRequested_.store(true);
    if (session_ && session_->view) {
        if (userStop) stamp_exit_reason(*session_->view, ExitReason::UserClosed);
        InterlockedExchange64(&session_->view->state, -1);
    }
    wakeRelaunch_.store(true);
    wakeRelaunch_.notify_one();
}

void Controller::stop(bool userStop) noexcept
{
    request_stop(userStop);
    if (relaunchWorker_.joinable()) relaunchWorker_.join();
    replacement_.reset();
    relaunchState_.store(RelaunchState::Idle);
    if (relaunchSettledEvent_) SetEvent(relaunchSettledEvent_);
    if (userStop && session_ && backend_is_pushflow(options_.backend)) {
        if (session_->child && WaitForSingleObject(session_->child, 0) == WAIT_OBJECT_0)
            await_exit_cleanup(250);
    }
}

bool Controller::is_fault_or_watchdog_exit() const noexcept
{
    const auto reason = exit_reason();
    return reason == ExitReason::Fault || reason == ExitReason::Watchdog;
}

bool Controller::can_recover() const noexcept
{
    return backend_supports_auto_fallback(options_.backend) && !fallbackRestarted_ && !stopRequested_.load() &&
        exit_reason() != ExitReason::UserClosed;
}

bool Controller::begin_async_fallback_relaunch(bool forceFaultOnTimeout) noexcept
{
    if (!can_recover() || (!forceFaultOnTimeout && !is_fault_or_watchdog_exit() && alive())) return false;
    if (forceFaultOnTimeout && session_ && session_->view) stamp_exit_reason(*session_->view, ExitReason::Fault);
    if (exit_reason() == ExitReason::UserClosed) return false;
    fallbackRestarted_ = true;
    if (relaunchSettledEvent_) ResetEvent(relaunchSettledEvent_);
    relaunchState_.store(RelaunchState::InFlight);
    wakeRelaunch_.store(true);
    wakeRelaunch_.notify_one();
    return true;
}

void Controller::run_fallback(LaunchOptions recovery)
{
    wakeRelaunch_.wait(false);
    if (stopRequested_.load() || exit_reason() == ExitReason::UserClosed) return;
    try {
        if (alive()) TerminateProcess(session_->child, 2);
        require(WaitForSingleObject(session_->child, 1000) == WAIT_OBJECT_0, "VFG faulted child reap");
        await_exit_cleanup(200);
        if (stopRequested_.load() || exit_reason() == ExitReason::UserClosed) return;
        if (backend_is_pushflow(recovery.backend)) {
            const auto marks = marker_dir();
            if (!marks.empty()) {
                std::error_code ec;
                fs::create_directories(marks, ec);
                std::ofstream marker(marks / L"pushflow-crash-quarantine.txt", std::ios::binary | std::ios::trunc);
                marker << "reason=controller_unexpected_exit\n";
                marker.close();
                if (!marker) write_event("event=controller_quarantine_write_failed\n");
            }
        }
        if (stopRequested_.load()) return;
        write_event("event=controller_auto_fallback_1x_relaunch\n");
        replacement_.reset(new Controller(recovery, true));
        if (stopRequested_.load()) replacement_.reset();
        relaunchState_.store(replacement_ ? RelaunchState::Ready : RelaunchState::Failed);
        if (relaunchSettledEvent_) SetEvent(relaunchSettledEvent_);
    } catch (const std::exception&) {
        relaunchState_.store(RelaunchState::Failed);
        if (relaunchSettledEvent_) SetEvent(relaunchSettledEvent_);
    }
}

RelaunchState Controller::poll_async_fallback_relaunch() noexcept
{
    const auto result = relaunchState_.load();
    if (result == RelaunchState::Ready && !stopRequested_.load()) {
        std::swap(session_, replacement_->session_);
        options_.multiplier = 1;
        clear_backend_fields(options_);
        ++launchEpoch_;
        sync_live_settings();
        relaunchState_.store(RelaunchState::Idle);
    }
    return result;
}

bool Controller::await_async_fallback_relaunch(DWORD timeoutMs) noexcept
{
    return relaunchSettledEvent_ && WaitForSingleObject(relaunchSettledEvent_, timeoutMs) == WAIT_OBJECT_0;
}

void Controller::set_fps_overlay(uint32_t pos) noexcept
{
    options_.fpsOverlay = pos;
    if (session_ && session_->view) InterlockedExchange(&session_->view->fpsOverlay, static_cast<LONG>(pos));
}
void Controller::set_low_latency(uint32_t on) noexcept
{
    options_.lowLatency = on;
    if (session_ && session_->view) InterlockedExchange(&session_->view->lowLatency, static_cast<LONG>(on));
}
void Controller::set_hide_from_capture(uint32_t hide) noexcept
{
    options_.hideFromCapture = hide ? 1u : 0u;
    if (session_ && session_->view)
        InterlockedExchange(&session_->view->hideFromCapture, static_cast<LONG>(options_.hideFromCapture));
}
void Controller::set_hud_lock(uint32_t on) noexcept
{
    options_.hudLock = on != 0;
    if (session_ && session_->view) InterlockedExchange(&session_->view->hudLock, static_cast<LONG>(options_.hudLock));
}
void Controller::set_pacing_repair(uint32_t on) noexcept
{
    options_.pacingRepair = on != 0;
    if (session_ && session_->view)
        InterlockedExchange(&session_->view->pacingRepair, static_cast<LONG>(options_.pacingRepair));
}
void Controller::set_adaptive_fg(uint32_t on) noexcept
{
    options_.adaptiveFg = on != 0;
    if (session_ && session_->view)
        InterlockedExchange(&session_->view->adaptiveFg, static_cast<LONG>(options_.adaptiveFg));
}
void Controller::set_adaptive_target_fps(uint32_t fps) noexcept
{
    options_.adaptiveTargetFps = fps <= LONG_MAX ? fps : 0;
    if (session_ && session_->view)
        InterlockedExchange(&session_->view->adaptiveTargetFps, static_cast<LONG>(options_.adaptiveTargetFps));
}
void Controller::set_dedup_mode(uint32_t mode) noexcept
{
    if (!is_valid_dedup_mode(mode)) return;
    options_.dedupMode = mode;
    if (session_ && session_->view) InterlockedExchange(&session_->view->dedupMode, static_cast<LONG>(mode));
}
void Controller::set_live_anchor(uint32_t on) noexcept
{
    if (!is_valid_live_anchor(on)) return;
    options_.liveAnchor = on;
    if (session_ && session_->view) InterlockedExchange(&session_->view->liveAnchor, static_cast<LONG>(on));
}
LiveSettings Controller::live_settings() const noexcept
{
    LiveSettings result{};
    auto* view = session_ && session_->view ? session_->view : nullptr;
    if (view) {
        result.dedupMode = static_cast<uint32_t>(InterlockedCompareExchange(&view->dedupMode, 0, 0));
        result.liveAnchor = static_cast<uint32_t>(InterlockedCompareExchange(&view->liveAnchor, 0, 0));
        result.experimentalXeMfgOptIn = static_cast<uint32_t>(InterlockedCompareExchange(&view->experimentalXeMfgOptIn, 0, 0));
        result.xefgStabilityMask = static_cast<uint32_t>(InterlockedCompareExchange(&view->xefgStabilityMask, 0, 0));
        result.sharedNvofEnabled = static_cast<uint32_t>(InterlockedCompareExchange(&view->sharedNvofEnabled, 0, 0));
        std::copy_n(std::begin(view->xessRuntimeDir), ARRAYSIZE(result.xessRuntimeDir), result.xessRuntimeDir);
        std::copy_n(std::begin(view->xemfgDllPath), ARRAYSIZE(result.xemfgDllPath), result.xemfgDllPath);
    } else {
        result.dedupMode = options_.dedupMode;
        result.liveAnchor = options_.liveAnchor;
        result.experimentalXeMfgOptIn = options_.experimentalXeMfgOptIn;
        result.xefgStabilityMask = options_.xefgStabilityMask;
        result.sharedNvofEnabled = options_.sharedNvofEnabled;
        std::copy(options_.xessRuntimeDir.begin(), options_.xessRuntimeDir.end(), result.xessRuntimeDir);
        std::copy(options_.xemfgDllPath.begin(), options_.xemfgDllPath.end(), result.xemfgDllPath);
    }
    return result;
}

void Controller::sync_live_settings() noexcept
{
    set_fps_overlay(options_.fpsOverlay);
    set_low_latency(options_.lowLatency);
    set_hide_from_capture(options_.hideFromCapture);
    set_hud_lock(options_.hudLock);
    set_pacing_repair(options_.pacingRepair);
    set_adaptive_fg(options_.adaptiveFg);
    set_adaptive_target_fps(options_.adaptiveTargetFps);
    set_dedup_mode(options_.dedupMode);
    set_live_anchor(options_.liveAnchor);
}

Controller::~Controller()
{
    stop();
    session_.reset();
    if (relaunchSettledEvent_) CloseHandle(relaunchSettledEvent_);
}

bool Controller::offer(const ipc::Offer& offer, const ipc::Shape& shape, LUID adapter) noexcept
{
    if (!session_ || !session_->view || offer.mapping.size() >= ARRAYSIZE(session_->view->mapping)) return false;
    auto* view = session_->view;
    const auto prior = InterlockedCompareExchange64(&view->state, 0, 0);
    if (prior != 0 && prior != 3) return false;
    view->generation = offer.generation;
    view->width = shape.width;
    view->height = shape.height;
    view->splitCompare = shape.splitCompare ? 1u : 0u;
    view->multiplier = std::clamp<uint32_t>(options_.multiplier, 1, max_multiplier);
    view->quality = static_cast<uint32_t>(options_.quality);
    view->adapter = adapter;
    InterlockedExchange(&view->fpsOverlay, static_cast<LONG>(std::min<uint32_t>(options_.fpsOverlay, 2)));
    InterlockedExchange(&view->lowLatency, static_cast<LONG>(options_.lowLatency ? 1u : 0u));
    InterlockedExchange(&view->gpuPriority, static_cast<LONG>(options_.gpuPriority ? 1u : 0u));
    InterlockedExchange(&view->hideFromCapture, static_cast<LONG>(options_.hideFromCapture ? 1u : 0u));
    InterlockedExchange(&view->dedupMode, static_cast<LONG>(options_.dedupMode));
    InterlockedExchange(&view->liveAnchor, static_cast<LONG>(options_.liveAnchor));
    InterlockedExchange(&view->experimentalXeMfgOptIn, static_cast<LONG>(options_.experimentalXeMfgOptIn));
    InterlockedExchange(&view->xefgStabilityMask, static_cast<LONG>(options_.xefgStabilityMask));
    InterlockedExchange(&view->sharedNvofEnabled, static_cast<LONG>(options_.sharedNvofEnabled));
    wcscpy_s(view->mapping, offer.mapping.c_str());
    if (!options_.monitorId.empty() && options_.monitorId.size() < ARRAYSIZE(view->monitorId)) {
        wcscpy_s(view->monitorId, options_.monitorId.c_str());
    } else {
        view->monitorId[0] = L'\0';
    }
    return InterlockedCompareExchange64(&view->state, 1, prior) == prior;
}
} // namespace nr::fg::vfg
