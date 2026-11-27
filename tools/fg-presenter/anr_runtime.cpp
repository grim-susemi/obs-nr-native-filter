// SPDX-License-Identifier: GPL-3.0-only
// Adapted from DosukaSOL and the PushFPS contributors, PushFPS 4e5c0491:
// 2026-11-22: port direct ANR ABI into the pinned OBS presenter loader.
#include "anr_runtime.h"
#include "anr_ngx_keys.h"
#include "fg/anr_dll_validator.h"
#include "ngx_parameters.h"
#include <wrl/client.h>
#include <array>
#include <bit>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <variant>

namespace nr::fg::vfg {
namespace {
namespace k = anr::keys;
using Result = NVSDK_NGX_Result;
using Parameter = NVSDK_NGX_Parameter;
using Handle = NVSDK_NGX_Handle;
using List = ID3D12GraphicsCommandList;
using Init = Result (NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
    NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using Populate = Result (NVSDK_CONV*)(Parameter*);
using Create = Result (NVSDK_CONV*)(List*, NVSDK_NGX_Feature, Parameter*, Handle**);
using Evaluate = Result (NVSDK_CONV*)(List*, const Handle*, Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using Release = Result (NVSDK_CONV*)(Handle*);
using Shutdown = Result (NVSDK_CONV*)(ID3D12Device*);

struct Outcome {
    Result result;
    DWORD seh{};
    bool ok(FILE* log, const char* stage) const
    {
        if (result == NVSDK_NGX_Result_Success) return true;
        if (log) fprintf(log, "event=anr_runtime_fault reason=%s result=0x%X seh=0x%lX\n",
            stage, static_cast<unsigned>(result), seh);
        return false;
    }
};

template<class F, class... Args>
__declspec(noinline) Outcome protected_call(F fn, Args... args)
{
    __try { return {fn(args...)}; }
    __except (GetExceptionCode() == 0xE06D7363u ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
        return {NVSDK_NGX_Result_FAIL_PlatformError, GetExceptionCode()};
    }
}

struct SharedAnrRuntime {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::unique_ptr<anr::PinnedAddon> addon;
    HMODULE module{};
    Populate populate{};
    Create create{};
    Evaluate evaluate{};
    Release release{};
    Shutdown shutdown{};
    bool quarantined{};
    ~SharedAnrRuntime() { if (module) FreeLibrary(module); }
};

std::mutex runtimeMutex;
std::map<std::pair<ID3D12Device*, std::wstring>, std::weak_ptr<SharedAnrRuntime>> runtimes;
} // namespace

struct AnrFeature::Impl {
    FILE* log;
    std::shared_ptr<SharedAnrRuntime> runtime;
    NgxParameters params;
    Handle* handle{};
    std::array<float, 16> projection{}, inverse{}, identity{};
};

AnrFeature::AnrFeature(FILE* log) : impl_(new Impl{log, {}, {}, nullptr, {}, {}, {}}) {}
AnrFeature::~AnrFeature()
{
    // Explicit close also runs while the caller still owns its GPU allocations.
    try {
        if (close()) return;
    } catch (...) {
        // A foreign C++ exception means ownership is just as uncertain as a failure result.
    }
    (void)impl_.release();
}

bool AnrFeature::close()
{
    auto& s = *impl_;
    std::lock_guard lock(runtimeMutex);
    if (!s.runtime) return true;
    if (std::exchange(s.runtime->quarantined, true)) return false;
    if (s.handle && !protected_call(s.runtime->release, s.handle).ok(s.log, "ReleaseFeature")) return false;
    s.handle = nullptr;
    if (s.runtime.use_count() == 1 &&
        !protected_call(s.runtime->shutdown, s.runtime->device.Get()).ok(s.log, "Shutdown1")) return false;
    s.runtime->quarantined = false;
    s.runtime.reset();
    return true;
}

unsigned AnrFeature::open(ID3D12Device* device, std::wstring_view path, bool optIn)
{
    auto& s = *impl_;
    auto validated = anr::validate_addon(path, optIn);
    if (validated.status != anr::ValidationStatus::Accepted) {
        if (s.log) fprintf(s.log, "event=%s reason=%s missing=%.*s\n",
            validated.status == anr::ValidationStatus::IncompleteAddonMissingCompanions ?
                "anr_addon_incomplete_fallback_1x" : "anr_validation_rejected",
            anr::status_name(validated.status), static_cast<int>(validated.relativePath.size()),
            validated.relativePath.empty() ? "" : validated.relativePath.data());
        return 0;
    }
    const auto key = std::pair{device, validated.addon->canonicalPath};
    std::lock_guard lock(runtimeMutex);
    std::erase_if(runtimes, [](const auto& entry) { return entry.second.expired(); });
    if (s.runtime = runtimes[key].lock(); s.runtime && s.runtime->quarantined) {
        if (s.log) fprintf(s.log, "event=anr_runtime_fault reason=runtime_quarantined\n");
        s.runtime.reset();
        return 0;
    }
    if (!s.runtime) {
        auto runtime = std::make_shared<SharedAnrRuntime>();
        runtime->device = device;
        runtime->addon = std::move(validated.addon);
        runtime->module = LoadLibraryExW(runtime->addon->canonicalPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!runtime->module) {
            if (s.log) fprintf(s.log, "event=anr_runtime_fault reason=LoadLibraryExW win32=%lu\n", GetLastError());
            return 0;
        }
        const auto symbol = [module = runtime->module](const char* name) { return GetProcAddress(module, name); };
        const auto init = std::bit_cast<Init>(symbol("NVSDK_NGX_D3D12_Init_Ext"));
        runtime->populate = std::bit_cast<Populate>(symbol("NVSDK_NGX_D3D12_PopulateParameters_Impl"));
        runtime->create = std::bit_cast<Create>(symbol("NVSDK_NGX_D3D12_CreateFeature"));
        runtime->evaluate = std::bit_cast<Evaluate>(symbol("NVSDK_NGX_D3D12_EvaluateFeature"));
        runtime->release = std::bit_cast<Release>(symbol("NVSDK_NGX_D3D12_ReleaseFeature"));
        runtime->shutdown = std::bit_cast<Shutdown>(symbol("NVSDK_NGX_D3D12_Shutdown1"));
        if (!init || !runtime->populate || !runtime->create || !runtime->evaluate ||
            !runtime->release || !runtime->shutdown) {
            if (s.log) fprintf(s.log, "event=anr_runtime_fault reason=export_resolution\n");
            return 0;
        }
        const auto directory = std::filesystem::path(key.second).parent_path();
        if (!protected_call(init, 1ull, directory.c_str(), device, NVSDK_NGX_Version_API,
            static_cast<const NVSDK_NGX_FeatureCommonInfo*>(nullptr)).ok(s.log, "Init_Ext")) return 0;
        s.runtime = runtime;
        runtimes[key] = runtime;
    }
    if (!protected_call(s.runtime->populate, &s.params).ok(s.log, "PopulateParameters_Impl")) return 0;
    unsigned maxGenerated{};
    if (s.params.Get(k::multiFrameCountMax, &maxGenerated) != NVSDK_NGX_Result_Success || !maxGenerated) {
        if (s.log) fprintf(s.log, "event=anr_runtime_fault reason=insufficient_capability\n");
        return 0;
    }
    return maxGenerated;
}

bool AnrFeature::create(List* list, unsigned width, unsigned height)
{
    auto& s = *impl_;
    std::lock_guard lock(runtimeMutex);
    if (s.runtime->quarantined) return false;
    auto& p = s.params;
    p.Set(k::creationNodeMask, 1u);
    p.Set(k::visibilityNodeMask, 1u);
    p.Set(k::width, width);
    p.Set(k::height, height);
    p.Set(k::internalWidth, width);
    p.Set(k::internalHeight, height);
    p.Set(k::backbufferFormat, static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
    p.Set(k::dynamicResolution, 0u);
    p.Set(k::resourceAlwaysProvidedFlags, k::alwaysProvided);
    p.Set(k::resourceNeverProvidedFlags, k::neverProvided);
    constexpr float nearPlane = 0.1f, farPlane = 1000.f;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float z = farPlane / (farPlane - nearPlane), w = -nearPlane * z;
    s.identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    s.projection = {1.f / aspect, 0, 0, 0, 0, 1, 0, 0, 0, 0, z, 1, 0, 0, w, 0};
    s.inverse = {aspect, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1.f / w, 0, 0, 1, -z / w};
    p.Set(k::cameraViewToClip, s.projection.data());
    p.Set(k::clipToCameraView, s.inverse.data());
    for (const auto* key : {k::clipToLensClip, k::clipToPrevClip, k::prevClipToClip}) p.Set(key, s.identity.data());
    p.Set(k::cameraNear, nearPlane);
    p.Set(k::cameraFar, farPlane);
    p.Set(k::cameraFov, 1.5707963f);
    p.Set(k::cameraAspectRatio, aspect);
    p.Set(k::cameraMotionIncluded, 1u);
    p.Set(k::depthInverted, 0u);
    p.Set(k::mvecJittered, 0u);
    p.Set(k::mvecInvalidValue, k::invalidMotionVector);
    for (unsigned i = 0; i < 3; ++i) {
        p.Set(k::cameraPosition[i], 0.f);
        p.Set(k::cameraRight[i], i == 0 ? 1.f : 0.f);
        p.Set(k::cameraUp[i], i == 1 ? 1.f : 0.f);
        p.Set(k::cameraForward[i], i == 2 ? 1.f : 0.f);
    }
    const auto result = protected_call(s.runtime->create, list, k::feature, &p, &s.handle);
    if (!result.ok(s.log, "CreateFeature")) return false;
    if (s.handle) return true;
    if (s.log) fprintf(s.log, "event=anr_runtime_fault reason=CreateFeature_null_handle\n");
    return false;
}

bool AnrFeature::evaluate(List* list, const AnrEvaluation& e)
{
    auto& s = *impl_;
    std::lock_guard lock(runtimeMutex);
    if (s.runtime->quarantined) return false;
    auto& p = s.params;
    p.Set(k::backbuffer, e.color);
    p.Set(k::mvecs, e.motion);
    p.Set(k::depth, e.depth);
    p.Set(k::outputInterpolated, e.output);
    p.Set(k::outputDisableInterpolation, e.disable);
    p.Set(k::backbufferFrameId, static_cast<unsigned long long>(e.frameId));
    p.Set(k::multiFrameCount, e.count);
    p.Set(k::multiFrameIndex, e.index);
    p.Set(k::reset, static_cast<unsigned>(e.reset));
    p.Set(k::mvecScaleX, 1.f);
    p.Set(k::mvecScaleY, 1.f);
    return protected_call(s.runtime->evaluate, list, s.handle, &p,
        static_cast<PFN_NVSDK_NGX_ProgressCallback>(nullptr)).ok(s.log, "EvaluateFeature");
}
} // namespace nr::fg::vfg
