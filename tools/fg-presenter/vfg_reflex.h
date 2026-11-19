// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nr::fg::vfg {
// NVIDIA Reflex + D3DKMT GPU priority helper (dynamic loading; safe no-op if unavailable)
namespace detail {
#pragma pack(push, 8)
struct NvSetSleepModeParamsV1 {
    uint32_t version;
    uint8_t bLowLatencyMode;
    uint8_t bLowLatencyBoost;
    uint32_t minimumIntervalUs;
    uint8_t bUseMarkersToOptimize;
    uint8_t bUseMinQueueTime;
    uint8_t rsvd[30];
};
struct NvLatencyMarkerParamsV1 {
    uint32_t version;
    uint64_t frameID;
    uint32_t markerType;
    uint8_t rsvd[64];
};
struct NvAsyncFrameMarkerParamsV1 {
    uint32_t version;
    uint64_t frameID;
    uint32_t markerType;
    uint64_t presentFrameID;
    uint8_t vendorInternal;
    uint8_t rsvd[55];
};
#pragma pack(pop)

template <class T> constexpr uint32_t make_nvapi_ver(uint32_t v) noexcept {
    return static_cast<uint32_t>(sizeof(T)) | (v << 16);
}

constexpr uint32_t idInitialize = 0x0150E828u;
constexpr uint32_t idUnload = 0xD22BDD7Eu;
constexpr uint32_t idSetSleepMode = 0xAC1CA9E0u;
constexpr uint32_t idSleep = 0x852CD1D2u;
constexpr uint32_t idSetLatencyMarker = 0xD9984C05u;
constexpr uint32_t idNotifyOobQueue = 0x03D6E8CBu;
constexpr uint32_t idSetAsyncMarker = 0x13C98F73u;
} // namespace detail

enum class ReflexMarker : uint32_t {
    SimulationStart = 0,
    SimulationEnd = 1,
    RenderSubmitStart = 2,
    RenderSubmitEnd = 3,
    PresentStart = 4,
    PresentEnd = 5,
    OutOfBandRenderSubmitStart = 9,
    OutOfBandRenderSubmitEnd = 10,
    OutOfBandPresentStart = 11,
    OutOfBandPresentEnd = 12
};

inline void set_gpu_process_priority(bool high, FILE* log) noexcept {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return;
    // D3DKMT_SCHEDULINGPRIORITYCLASS: NORMAL=2, ABOVE_NORMAL=3, HIGH=4, REALTIME=5.
    // Keep VFG at ABOVE_NORMAL(3) so upstream OBS Native NR(4) is never preempted.
    using SetPrioFn = LONG(NTAPI*)(HANDLE, int);
    using GetPrioFn = LONG(NTAPI*)(HANDLE, int*);
    auto setFn = reinterpret_cast<SetPrioFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"));
    auto getFn = reinterpret_cast<GetPrioFn>(GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass"));
    if (!setFn) return;
    const int req = high ? 3 : 2;
    const LONG st = setFn(GetCurrentProcess(), req);
    int actual = -1;
    if (getFn) (void)getFn(GetCurrentProcess(), &actual);
    if (log) fprintf(log, "event=gpu_priority requested=%d actual=%d ntstatus=0x%08lX\n", req, actual, static_cast<unsigned long>(st));
}

class ReflexContext final {
    using QueryFn = void*(__cdecl*)(uint32_t);
    using InitFn = int(__cdecl*)();
    using UnloadFn = int(__cdecl*)();
    using SetSleepFn = int(__cdecl*)(IUnknown*, const detail::NvSetSleepModeParamsV1*);
    using SleepFn = int(__cdecl*)(IUnknown*);
    using MarkerFn = int(__cdecl*)(IUnknown*, const detail::NvLatencyMarkerParamsV1*);
    using NotifyOobFn = int(__cdecl*)(ID3D12CommandQueue*, uint32_t);
    using AsyncMarkerFn = int(__cdecl*)(ID3D12CommandQueue*, const detail::NvAsyncFrameMarkerParamsV1*);

    HMODULE nvapi_{};
    InitFn init_{};
    UnloadFn unload_{};
    SetSleepFn setSleep_{};
    SleepFn sleep_{};
    MarkerFn marker_{};
    NotifyOobFn notifyOob_{};
    AsyncMarkerFn asyncMarker_{};
    IUnknown* device_{};
    ID3D12CommandQueue* oobQueue_{};
    bool initialized_{};
    bool enabled_{};
    bool sleepDisabledFallback_{};
    unsigned slowSleepStreak_{};
    FILE* log_{};

public:
    ReflexContext(ID3D12Device* dev, ID3D12CommandQueue* drawQueue, FILE* log) : device_(dev), oobQueue_(drawQueue), log_(log) {
        nvapi_ = LoadLibraryW(L"nvapi64.dll");
        if (!nvapi_) return;
        auto query = reinterpret_cast<QueryFn>(GetProcAddress(nvapi_, "nvapi_QueryInterface"));
        if (!query) return;
        init_ = reinterpret_cast<InitFn>(query(detail::idInitialize));
        unload_ = reinterpret_cast<UnloadFn>(query(detail::idUnload));
        setSleep_ = reinterpret_cast<SetSleepFn>(query(detail::idSetSleepMode));
        sleep_ = reinterpret_cast<SleepFn>(query(detail::idSleep));
        marker_ = reinterpret_cast<MarkerFn>(query(detail::idSetLatencyMarker));
        notifyOob_ = reinterpret_cast<NotifyOobFn>(query(detail::idNotifyOobQueue));
        asyncMarker_ = reinterpret_cast<AsyncMarkerFn>(query(detail::idSetAsyncMarker));
        if (!init_ || init_() != 0) return;
        initialized_ = true;
        if (notifyOob_ && oobQueue_) {
            const int st = notifyOob_(oobQueue_, 1u);
            if (log_) fprintf(log_, "event=reflex_oob_queue status=%d\n", st);
        }
    }

    ~ReflexContext() {
        if (initialized_ && enabled_) set_sleep_mode(false);
        if (initialized_ && unload_) (void)unload_();
        if (nvapi_) FreeLibrary(nvapi_);
    }

    ReflexContext(const ReflexContext&) = delete;
    ReflexContext& operator=(const ReflexContext&) = delete;

    void set_sleep_mode(bool enable) noexcept {
        if (!initialized_ || !setSleep_ || !device_) return;
        detail::NvSetSleepModeParamsV1 p{};
        p.version = detail::make_nvapi_ver<detail::NvSetSleepModeParamsV1>(1);
        p.bLowLatencyMode = enable ? 1u : 0u;
        p.bLowLatencyBoost = 0u; // Veyra 2.1.6: Off/On without Boost
        p.minimumIntervalUs = 0u;
        p.bUseMarkersToOptimize = enable ? 1u : 0u;
        const int st = setSleep_(device_, &p);
        enabled_ = enable && st == 0;
        if (log_) fprintf(log_, "event=reflex_mode enabled=%d status=%d\n", enabled_ ? 1 : 0, st);
    }

    void sleep(uint64_t nowNs, uint64_t(*clockFn)() noexcept) noexcept {
        if (!initialized_ || !enabled_ || sleepDisabledFallback_ || !sleep_ || !device_) return;
        const uint64_t t0 = nowNs;
        (void)sleep_(device_);
        const uint64_t dtMs = (clockFn() - t0) / 1000000ULL;
        if (dtMs > 50ULL) {
            if (++slowSleepStreak_ >= 3u) {
                sleepDisabledFallback_ = true;
                if (log_) fprintf(log_, "event=reflex_sleep_fallback reason=long_sleep dt_ms=%llu\n", static_cast<unsigned long long>(dtMs));
            }
        } else {
            slowSleepStreak_ = 0;
        }
    }

    void marker(ReflexMarker kind, uint64_t frameId) noexcept {
        if (!initialized_ || !enabled_ || !marker_ || !device_) return;
        detail::NvLatencyMarkerParamsV1 p{};
        p.version = detail::make_nvapi_ver<detail::NvLatencyMarkerParamsV1>(1);
        p.frameID = frameId;
        p.markerType = static_cast<uint32_t>(kind);
        (void)marker_(device_, &p);
    }

    void oob_marker(ReflexMarker kind, uint64_t frameId, uint64_t presentFrameId) noexcept {
        if (!initialized_ || !enabled_ || !asyncMarker_ || !oobQueue_) return;
        detail::NvAsyncFrameMarkerParamsV1 p{};
        p.version = detail::make_nvapi_ver<detail::NvAsyncFrameMarkerParamsV1>(1);
        p.frameID = frameId;
        p.markerType = static_cast<uint32_t>(kind);
        p.presentFrameID = presentFrameId;
        (void)asyncMarker_(oobQueue_, &p);
    }
};
} // namespace nr::fg::vfg
