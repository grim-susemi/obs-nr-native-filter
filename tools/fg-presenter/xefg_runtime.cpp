// SPDX-License-Identifier: GPL-3.0-only
#include "xefg_runtime.h"
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <vector>

namespace nr::fg::vfg {
namespace {
constexpr wchar_t fg_library[] = L"libxess_fg.dll";
constexpr wchar_t xell_library[] = L"libxell.dll";
constexpr ULONGLONG fg_min_bytes = 1ull << 20;
constexpr ULONGLONG fg_max_bytes = 96ull << 20;
constexpr ULONGLONG xell_min_bytes = 64ull << 10;
constexpr ULONGLONG xell_max_bytes = 32ull << 20;
// Test fixtures are small unsigned modules; size admission is not the security
// boundary (the Intel signature and the pinned hash are), so only the test build
// relaxes the minimum. Shipping still requires the official image sizes.
#ifdef NR_FG_TEST_MOCK_XESS
constexpr ULONGLONG fg_min_accepted = 16ull << 10;
constexpr ULONGLONG xell_min_accepted = 16ull << 10;
#else
constexpr ULONGLONG fg_min_accepted = fg_min_bytes;
constexpr ULONGLONG xell_min_accepted = xell_min_bytes;
#endif

#ifdef NR_FG_TEST_MOCK_XESS
std::string narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
        bytes, nullptr, nullptr);
    return result;
}

std::string env_utf8(const wchar_t* name)
{
    wchar_t value[256]{};
    const DWORD length = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    if (!length || length >= ARRAYSIZE(value)) return {};
    return narrow(value);
}
#endif

std::string sha256_file(HANDLE file, uint64_t size)
{
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    struct CloseAlgorithm { BCRYPT_ALG_HANDLE handle; ~CloseAlgorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); } }
        closeAlgorithm{algorithm};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) return {};
    struct CloseHash { BCRYPT_HASH_HANDLE handle; ~CloseHash() { if (handle) BCryptDestroyHash(handle); } } closeHash{hash};
    std::vector<unsigned char> buffer(1u << 20);
    uint64_t remaining = size;
    while (remaining) {
        const auto take = static_cast<DWORD>(std::min<uint64_t>(remaining, buffer.size()));
        DWORD read{};
        if (!ReadFile(file, buffer.data(), take, &read, nullptr) || read != take) return {};
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) return {};
        remaining -= read;
    }
    unsigned char digest[32]{};
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) return {};
    const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(sizeof(digest) * 2);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

bool equals_ascii_case_insensitive(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool intel_signature(HANDLE file, const std::wstring& path)
{
    WINTRUST_FILE_INFO info{};
    info.cbStruct = sizeof(info);
    info.pcwszFilePath = path.c_str();
    info.hFile = file;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &data);
    std::wstring subject;
    if (status == ERROR_SUCCESS) {
        auto* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        const auto* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain && signer->pasCertChain[0].pCert) {
            wchar_t name[512]{};
            if (CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                    nullptr, name, ARRAYSIZE(name)))
                subject = name;
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr, &action, &data);
    return status == ERROR_SUCCESS && _wcsicmp(subject.c_str(), L"Intel Corporation") == 0;
}

struct PinnedFile {
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::wstring finalPath;
    std::string sha256;
    ~PinnedFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    PinnedFile() = default;
    PinnedFile(const PinnedFile&) = delete;
    PinnedFile& operator=(const PinnedFile&) = delete;
};

enum class PinStatus { Ok, Missing, UntrustedPath, SizeOutOfRange, IoFailure };
struct SizeBounds {
    uint64_t min, max;
};

PinStatus pin_library(const std::filesystem::path& path, SizeBounds bounds, PinnedFile& out)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return PinStatus::Missing;
    out.handle = file;
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!GetFileInformationByHandle(file, &identity)) return PinStatus::IoFailure;
    if (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return PinStatus::UntrustedPath;
    const ULONGLONG size = (static_cast<ULONGLONG>(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow;
    if (size < bounds.min || size > bounds.max) return PinStatus::SizeOutOfRange;
    std::vector<wchar_t> finalPath(32768);
    const DWORD length = GetFinalPathNameByHandleW(file, finalPath.data(),
        static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED);
    if (!length || length >= finalPath.size()) return PinStatus::IoFailure;
    out.finalPath.assign(finalPath.data(), length);
    out.sha256 = sha256_file(file, size);
    return out.sha256.empty() ? PinStatus::IoFailure : PinStatus::Ok;
}

template<class T>
T resolve(HMODULE module, const char* name) noexcept
{
    return std::bit_cast<T>(GetProcAddress(module, name));
}

struct Pins {
    std::string fg, xell;
    bool fixture{};
};

Pins accepted_pins()
{
#ifdef NR_FG_TEST_MOCK_XESS
    if (env_utf8(L"OBS_NR_TEST_XESS_TRUST_MOCK") == "1")
        return {env_utf8(L"OBS_NR_TEST_XESS_FG_SHA256"), env_utf8(L"OBS_NR_TEST_XESS_XELL_SHA256"), true};
#endif
    return {std::string(xefg_pinned_sha256), std::string(xell_pinned_sha256), false};
}
} // namespace

const char* xefg_load_status_name(XeFgLoadStatus status) noexcept
{
    switch (status) {
    case XeFgLoadStatus::Ok: return "ok";
    case XeFgLoadStatus::MissingLibrary: return "missing_library";
    case XeFgLoadStatus::UntrustedPath: return "untrusted_path";
    case XeFgLoadStatus::SignatureRejected: return "signature_rejected";
    case XeFgLoadStatus::HashMismatch: return "hash_mismatch";
    case XeFgLoadStatus::ExportMissing: return "export_missing";
    case XeFgLoadStatus::LoadFailed: return "load_failed";
    }
    return "unknown";
}

struct XeFgRuntime::Impl {
    HMODULE fg{};
    HMODULE xell{};
    XeFgExports api{};
    std::wstring directory;
    std::string detail{"none"};
    bool fixture{};
    xefg_swapchain_version_t swapchainVersion{};
    xell_version_t xellVersion{};
};

XeFgRuntime::XeFgRuntime() : impl_(new Impl) {}
XeFgRuntime::~XeFgRuntime() { close(); }

bool XeFgRuntime::loaded() const noexcept
{
    const auto& a = impl_->api;
    return impl_->fg && impl_->xell && a.get_properties && a.destroy && a.set_enabled &&
        a.set_latency_reduction && a.tag_frame_constants && a.set_present_id &&
        a.get_last_present_status && a.d3d12_create_context && a.d3d12_init_from_desc &&
        a.d3d12_get_swapchain_ptr && a.d3d12_tag_resource && a.xell_create &&
        a.xell_set_sleep_mode && a.xell_add_marker && a.xell_sleep && a.xell_destroy;
}

const XeFgExports& XeFgRuntime::api() const noexcept { return impl_->api; }
std::wstring_view XeFgRuntime::directory() const noexcept { return impl_->directory; }
bool XeFgRuntime::test_fixture() const noexcept { return impl_->fixture; }
const char* XeFgRuntime::detail() const noexcept { return impl_->detail.c_str(); }
xefg_swapchain_version_t XeFgRuntime::swapchain_version() const noexcept { return impl_->swapchainVersion; }
xell_version_t XeFgRuntime::xell_version() const noexcept { return impl_->xellVersion; }

void XeFgRuntime::close() noexcept
{
    auto& s = *impl_;
    if (s.fg) { FreeLibrary(s.fg); s.fg = nullptr; }
    if (s.xell) { FreeLibrary(s.xell); s.xell = nullptr; }
    s.api = {};
    s.directory.clear();
    s.detail = "none";
    s.fixture = false;
    s.swapchainVersion = {};
    s.xellVersion = {};
}

XeFgLoadStatus XeFgRuntime::open(std::wstring_view directory, FILE* log)
{
    auto& s = *impl_;
    close();
    const auto fail = [&](XeFgLoadStatus status, const char* detail) {
        s.detail = detail;
        if (log) fprintf(log, "event=xefg_runtime_rejected reason=%s gate=%s directory=%.*ls\n",
            xefg_load_status_name(status), detail, static_cast<int>(directory.size()), directory.data());
        close();
        s.detail = detail;
        return status;
    };
    if (directory.empty()) return fail(XeFgLoadStatus::MissingLibrary, "directory");
    const std::filesystem::path base{std::wstring(directory)};
    if (!base.is_absolute()) return fail(XeFgLoadStatus::UntrustedPath, "relative_directory");
    s.directory = base.wstring();
    const auto pins = accepted_pins();
    s.fixture = pins.fixture;
    if (pins.fixture && (pins.fg.empty() || pins.xell.empty()))
        return fail(XeFgLoadStatus::HashMismatch, "mock_hash_missing");

    PinnedFile fg, xell;
    if (pin_library(base / fg_library, {fg_min_accepted, fg_max_bytes}, fg) != PinStatus::Ok)
        return fail(XeFgLoadStatus::MissingLibrary, "libxess_fg");
    if (pin_library(base / xell_library, {xell_min_accepted, xell_max_bytes}, xell) != PinStatus::Ok)
        return fail(XeFgLoadStatus::MissingLibrary, "libxell");
    if (!pins.fixture) {
        if (!intel_signature(fg.handle, fg.finalPath))
            return fail(XeFgLoadStatus::SignatureRejected, "libxess_fg");
        if (!intel_signature(xell.handle, xell.finalPath))
            return fail(XeFgLoadStatus::SignatureRejected, "libxell");
    }
    if (!equals_ascii_case_insensitive(fg.sha256, pins.fg))
        return fail(XeFgLoadStatus::HashMismatch, "libxess_fg");
    if (!equals_ascii_case_insensitive(xell.sha256, pins.xell))
        return fail(XeFgLoadStatus::HashMismatch, "libxell");

    // The pinned handles keep both files undeletable and unwritable until the
    // images are mapped, so the admitted bytes and the loaded image cannot differ.
    s.xell = LoadLibraryExW(xell.finalPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.xell) return fail(XeFgLoadStatus::LoadFailed, "libxell_load");
    s.fg = LoadLibraryExW(fg.finalPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.fg) return fail(XeFgLoadStatus::LoadFailed, "libxess_fg_load");
    {
        wchar_t image[32768]{};
        const DWORD length = GetModuleFileNameW(s.fg, image, static_cast<DWORD>(ARRAYSIZE(image)));
        std::error_code error;
        const bool same = length && length < ARRAYSIZE(image) &&
            std::filesystem::equivalent(std::filesystem::path(fg.finalPath),
                std::filesystem::path(std::wstring(image, length)), error) && !error;
        if (!same) return fail(XeFgLoadStatus::LoadFailed, "loaded_path_mismatch");
    }

    auto& a = s.api;
    a.get_properties = resolve<decltype(a.get_properties)>(s.fg, "xefgSwapChainGetProperties");
    a.destroy = resolve<decltype(a.destroy)>(s.fg, "xefgSwapChainDestroy");
    a.set_enabled = resolve<decltype(a.set_enabled)>(s.fg, "xefgSwapChainSetEnabled");
    a.set_latency_reduction = resolve<decltype(a.set_latency_reduction)>(s.fg, "xefgSwapChainSetLatencyReduction");
    a.tag_frame_constants = resolve<decltype(a.tag_frame_constants)>(s.fg, "xefgSwapChainTagFrameConstants");
    a.set_present_id = resolve<decltype(a.set_present_id)>(s.fg, "xefgSwapChainSetPresentId");
    a.get_last_present_status = resolve<decltype(a.get_last_present_status)>(s.fg,
        "xefgSwapChainGetLastPresentStatus");
    a.d3d12_create_context = resolve<decltype(a.d3d12_create_context)>(s.fg,
        "xefgSwapChainD3D12CreateContext");
    a.d3d12_init_from_desc = resolve<decltype(a.d3d12_init_from_desc)>(s.fg,
        "xefgSwapChainD3D12InitFromSwapChainDesc");
    a.d3d12_get_swapchain_ptr = resolve<decltype(a.d3d12_get_swapchain_ptr)>(s.fg,
        "xefgSwapChainD3D12GetSwapChainPtr");
    a.d3d12_tag_resource = resolve<decltype(a.d3d12_tag_resource)>(s.fg,
        "xefgSwapChainD3D12TagFrameResource");
    a.xell_create = resolve<decltype(a.xell_create)>(s.xell, "xellD3D12CreateContext");
    a.xell_set_sleep_mode = resolve<decltype(a.xell_set_sleep_mode)>(s.xell, "xellSetSleepMode");
    a.xell_add_marker = resolve<decltype(a.xell_add_marker)>(s.xell, "xellAddMarkerData");
    a.xell_sleep = resolve<decltype(a.xell_sleep)>(s.xell, "xellSleep");
    a.xell_destroy = resolve<decltype(a.xell_destroy)>(s.xell, "xellDestroyContext");
    a.get_version = resolve<decltype(a.get_version)>(s.fg, "xefgSwapChainGetVersion");
    a.xell_get_version = resolve<decltype(a.xell_get_version)>(s.xell, "xellGetVersion");
    a.set_ui_composition_state = resolve<decltype(a.set_ui_composition_state)>(s.fg,
        "xefgSwapChainSetUiCompositionState");
    a.set_num_interpolated_frames = resolve<decltype(a.set_num_interpolated_frames)>(s.fg,
        "xefgSwapChainSetNumInterpolatedFrames");
    a.set_scene_change_threshold = resolve<decltype(a.set_scene_change_threshold)>(s.fg,
        "xefgSwapChainSetSceneChangeThreshold");
    a.set_logging_callback = resolve<decltype(a.set_logging_callback)>(s.fg,
        "xefgSwapChainSetLoggingCallback");
    a.d3d12_build_pipelines = resolve<decltype(a.d3d12_build_pipelines)>(s.fg,
        "xefgSwapChainD3D12BuildPipelines");
    a.d3d12_set_descriptor_heap = resolve<decltype(a.d3d12_set_descriptor_heap)>(s.fg,
        "xefgSwapChainD3D12SetDescriptorHeap");
    a.d3d12_get_properties = resolve<decltype(a.d3d12_get_properties)>(s.fg,
        "xefgSwapChainD3D12GetProperties");
    a.d3d12_get_initialization_parameters = resolve<decltype(a.d3d12_get_initialization_parameters)>(s.fg,
        "xefgSwapChainD3D12GetInitializationParameters");
    a.d3d12_update_external_heap = resolve<decltype(a.d3d12_update_external_heap)>(s.fg,
        "xefgSwapChainD3D12UpdateExternalHeapOnResize");
    a.enable_debug_feature = resolve<decltype(a.enable_debug_feature)>(s.fg,
        "xefgSwapChainEnableDebugFeature");
    a.xell_get_sleep_mode = resolve<decltype(a.xell_get_sleep_mode)>(s.xell, "xellGetSleepMode");
    a.xell_set_logging_callback = resolve<decltype(a.xell_set_logging_callback)>(s.xell,
        "xellSetLoggingCallback");
    a.xell_get_frames_reports = resolve<decltype(a.xell_get_frames_reports)>(s.xell,
        "xellGetFramesReports");
    if (!loaded()) return fail(XeFgLoadStatus::ExportMissing, "required_export");

    if (a.get_version) (void)a.get_version(&s.swapchainVersion);
    if (a.xell_get_version) (void)a.xell_get_version(&s.xellVersion);
    if (log) {
        fprintf(log,
            "event=xefg_runtime_loaded dir=%ls fg_version=%u.%u.%u xell_version=%u.%u.%u "
            "signature=%s fg_sha256=%.8s xell_sha256=%.8s\n",
            s.directory.c_str(), s.swapchainVersion.major, s.swapchainVersion.minor,
            s.swapchainVersion.patch, s.xellVersion.major, s.xellVersion.minor, s.xellVersion.patch,
            pins.fixture ? "mock" : "intel", fg.sha256.c_str(), xell.sha256.c_str());
    }
    return XeFgLoadStatus::Ok;
}
} // namespace nr::fg::vfg
