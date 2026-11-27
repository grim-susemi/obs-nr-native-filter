// SPDX-License-Identifier: GPL-3.0-only
#include "xemfg_runtime.h"
#include "fg_vfg_session.h"
#include "xemfg_companion.h"
#include <nvsdk_ngx_defs.h>
#include <windows.h>
#include <algorithm>
#include <bit>
#include <type_traits>
#include <utility>

namespace nr::fg::vfg {
namespace {
template<class T> struct Outcome {
    T value{};
    DWORD seh{};
};

template<class F, class... Args>
__declspec(noinline) Outcome<std::invoke_result_t<F, Args...>> protected_call(F fn, Args... args)
{
    using T = std::invoke_result_t<F, Args...>;
    __try { return {fn(args...)}; }
    __except (GetExceptionCode() == 0xE06D7363u ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
        return {T{}, GetExceptionCode()};
    }
}
} // namespace

struct XeMfgRuntime::Impl {
    FILE* log{};
    ID3D12Device* device{};
    std::unique_ptr<xemfg::PinnedProvider> pinned;
    HMODULE module{};
    const MfgXeMFGProviderApi* api{};
    uint32_t maxGenerated{};
    bool initialized{}, quarantined{};
    std::wstring companionDirectory;

    ~Impl()
    {
        if (module && !quarantined) FreeLibrary(module);
    }

    void quarantine(const char* stage, DWORD seh)
    {
        quarantined = true;
        if (log) fprintf(log, "event=xemfg_runtime_fault reason=%s seh=0x%lX quarantine=1\n", stage, seh);
    }
};

XeMfgRuntime::XeMfgRuntime() = default;

XeMfgRuntime::~XeMfgRuntime()
{
    try {
        if (close()) return;
    } catch (...) {
        // An unconfirmed shutdown leaves the image mapped on purpose.
    }
    (void)impl_.release();
}

bool XeMfgRuntime::loaded() const noexcept
{
    return impl_ && impl_->api && !impl_->quarantined;
}

bool XeMfgRuntime::quarantined() const noexcept { return impl_ && impl_->quarantined; }
uint32_t XeMfgRuntime::max_generated() const noexcept { return impl_ ? impl_->maxGenerated : 0; }
std::wstring_view XeMfgRuntime::companion_directory() const noexcept
{
    return impl_ ? std::wstring_view(impl_->companionDirectory) : std::wstring_view{};
}
std::string_view XeMfgRuntime::sha256() const noexcept
{
    return impl_ && impl_->pinned ? std::string_view(impl_->pinned->sha256) : std::string_view{};
}

const MfgXeMFGProviderApi& XeMfgRuntime::api() const noexcept
{
    static const MfgXeMFGProviderApi empty{};
    return impl_ && impl_->api ? *impl_->api : empty;
}

bool XeMfgRuntime::open(std::wstring_view path, bool optIn, std::wstring_view xessRuntimeDir,
    ID3D12Device* device, FILE* log)
{
    close();
    if (!device) return false;
    auto impl = std::make_unique<Impl>();
    impl->log = log;
    impl->device = device;
    auto gate = make_companion_gate(log);
    auto validated = xemfg::validate_provider(path, optIn, xessRuntimeDir, *gate);
    if (validated.status != xemfg::ValidationStatus::Accepted) {
        if (log) fprintf(log, "event=xemfg_validation_rejected reason=%s detail=%ls\n",
            xemfg::status_name(validated.status), validated.detail.c_str());
        return false;
    }
    impl->pinned = std::move(validated.provider);
    impl->companionDirectory = impl->pinned->companionDirectory;
    // The pinned handle keeps the admitted bytes undeletable while the loader maps them.
    impl->module = LoadLibraryExW(impl->pinned->canonicalPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!impl->module) {
        if (log) fprintf(log, "event=xemfg_runtime_fault reason=LoadLibraryExW win32=%lu\n", GetLastError());
        return false;
    }
    const auto symbol = GetProcAddress(impl->module, MFG_XEMFG_PROVIDER_GET_API);
    if (!symbol) {
        if (log) fprintf(log, "event=xemfg_runtime_fault reason=export_resolution\n");
        return false;
    }
    const auto get_api = std::bit_cast<PFN_MfgXeMFGProviderGetApi>(symbol);
    const auto table = protected_call(get_api, static_cast<uint32_t>(MFG_XEMFG_PROVIDER_API_VERSION));
    if (table.seh) {
        impl->quarantine(MFG_XEMFG_PROVIDER_GET_API, table.seh);
        impl_ = std::move(impl);
        return false;
    }
    const auto* api = table.value;
    if (!api || api->version != static_cast<uint32_t>(MFG_XEMFG_PROVIDER_API_VERSION) ||
        api->size < MFG_XEMFG_PROVIDER_API_SIZE_D3D12 || !api->Refusal || !api->MaxGeneratedFrames ||
        !api->D3D12Init || !api->D3D12CreateFeature || !api->D3D12Evaluate ||
        !api->D3D12ReleaseFeature || !api->D3D12Shutdown) {
        if (log) fprintf(log, "event=xemfg_runtime_fault reason=api_too_old size=%u version=%u\n",
            api ? api->size : 0u, api ? api->version : 0u);
        return false;
    }
    impl->api = api;
    char refusal[256]{};
    const auto refused = protected_call(api->Refusal, refusal, static_cast<uint32_t>(sizeof refusal));
    if (refused.seh) {
        impl->quarantine("Refusal", refused.seh);
        impl_ = std::move(impl);
        return false;
    }
    refusal[sizeof refusal - 1] = '\0';
    if (refused.value != 0) {
        if (log) fprintf(log, "event=xemfg_refused reason=%s fallback=1x\n", refusal);
        return false;
    }
    const auto maxGenerated = protected_call(api->MaxGeneratedFrames);
    if (maxGenerated.seh) {
        impl->quarantine("MaxGeneratedFrames", maxGenerated.seh);
        impl_ = std::move(impl);
        return false;
    }
    if (!maxGenerated.value) {
        if (log) fprintf(log, "event=xemfg_runtime_fault reason=insufficient_capability\n");
        return false;
    }
    impl->maxGenerated = std::min(maxGenerated.value, max_multiplier - 1);
    const auto initialized = protected_call(api->D3D12Init, device);
    if (initialized.seh) {
        impl->quarantine("D3D12Init", initialized.seh);
        impl_ = std::move(impl);
        return false;
    }
    if (initialized.value != NVSDK_NGX_Result_Success) {
        if (log) fprintf(log, "event=xemfg_runtime_fault reason=d3d12_init result=0x%X\n",
            static_cast<unsigned>(initialized.value));
        return false;
    }
    impl->initialized = true;
    if (log) fprintf(log,
        "event=xemfg_runtime_loaded provider=%ls sha256=%s companions=%ls max_generated=%u\n",
        impl->pinned->canonicalPath.c_str(), impl->pinned->sha256.c_str(),
        impl->companionDirectory.c_str(), impl->maxGenerated);
    impl_ = std::move(impl);
    return true;
}

bool XeMfgRuntime::create_feature(ID3D12GraphicsCommandList* list, NgxParameters& parameters, void*& feature)
{
    feature = nullptr;
    if (!impl_ || impl_->quarantined || !impl_->api) return false;
    const auto result = protected_call(impl_->api->D3D12CreateFeature, list, &parameters, &feature);
    if (result.seh) {
        impl_->quarantine("D3D12CreateFeature", result.seh);
        return false;
    }
    if (result.value != NVSDK_NGX_Result_Success || !feature) {
        if (impl_->log) fprintf(impl_->log, "event=xemfg_runtime_fault reason=create_feature result=0x%X\n",
            static_cast<unsigned>(result.value));
        return false;
    }
    return true;
}

bool XeMfgRuntime::evaluate(ID3D12GraphicsCommandList* list, void* feature, NgxParameters& parameters)
{
    if (!impl_ || impl_->quarantined || !feature) return false;
    const auto result = protected_call(impl_->api->D3D12Evaluate, list, feature, &parameters);
    if (result.seh) {
        impl_->quarantine("D3D12Evaluate", result.seh);
        return false;
    }
    if (result.value != NVSDK_NGX_Result_Success) {
        if (impl_->log) fprintf(impl_->log, "event=xemfg_evaluate_rejected result=0x%X\n",
            static_cast<unsigned>(result.value));
        return false;
    }
    return true;
}

bool XeMfgRuntime::release_feature(void* feature)
{
    if (!feature) return true;
    if (!impl_ || impl_->quarantined) return false;
    const auto result = protected_call(impl_->api->D3D12ReleaseFeature, feature);
    if (result.seh) {
        impl_->quarantine("D3D12ReleaseFeature", result.seh);
        return false;
    }
    if (result.value != NVSDK_NGX_Result_Success) {
        impl_->quarantine("D3D12ReleaseFeature_refused", 0);
        return false;
    }
    return true;
}

bool XeMfgRuntime::close() noexcept
{
    if (!impl_) return true;
    auto& s = *impl_;
    if (s.quarantined) return false;
    try {
        if (s.initialized) {
            const auto result = protected_call(s.api->D3D12Shutdown);
            s.initialized = false;
            if (result.seh) {
                s.quarantine("D3D12Shutdown", result.seh);
                return false;
            }
            if (result.value != NVSDK_NGX_Result_Success) {
                s.quarantine("D3D12Shutdown_refused", 0);
                return false;
            }
        }
        if (s.module) {
            FreeLibrary(s.module);
            s.module = nullptr;
        }
        impl_.reset();
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace nr::fg::vfg
