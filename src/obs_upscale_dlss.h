// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <cmath>
#ifdef _WIN32
#include "nr_flow_broker.h"
#include <memory>
#include <optional>
#endif

struct gs_texture;
using gs_texture_t = struct gs_texture;
struct ObsUpscaleDlss;

#ifdef _WIN32
// Single-owner decision for SR's optical-flow stage. NR owns motion analysis
// whenever susemi_nr_filter is active on the captured source chain; only a
// genuine consumer fallback (no NR, disabled, or unable to process) may run
// the local D3D11 NVOF session.
enum class SrFlowMode : uint32_t { LocalFallback = 0, UseShared = 1, ForbidLocal = 2 };

struct SrFlowInput {
    bool nrPresent = false;  // susemi_nr_filter found on the parent's filter chain
    bool nrEnabled = false;  // that filter source is enabled
    bool nrReady = false;    // NR admission is Ready: it can produce NR frames
    bool displayFound = false;
    bool pairAvailable = false;
    nr::FlowOwnerPolicy owner = nr::FlowOwnerPolicy::Unavailable;
    nr::FlowProvenance provenance = nr::FlowProvenance::Unknown;
};

inline SrFlowMode sr_flow_mode(const SrFlowInput& input) noexcept
{
    if (!input.nrPresent || !input.nrEnabled || !input.nrReady)
        return SrFlowMode::LocalFallback;
    if (!input.displayFound)
        return SrFlowMode::ForbidLocal; // NR owns the chain but has no displayed provenance yet
    switch (input.owner) {
    case nr::FlowOwnerPolicy::NrOwned:
        return input.provenance == nr::FlowProvenance::Valid && input.pairAvailable
            ? SrFlowMode::UseShared : SrFlowMode::ForbidLocal;
    case nr::FlowOwnerPolicy::ConsumerFallback:
        return SrFlowMode::LocalFallback;
    case nr::FlowOwnerPolicy::Unavailable:
        return SrFlowMode::ForbidLocal;
    }
    return SrFlowMode::ForbidLocal;
}

// Shared ring vectors are current->previous in guide (analysis) pixels with the
// broker's mvScale, while SR's NGX motion texture holds output pixels. Apply
// guide -> output exactly once; NGX's MV_Scale then normalizes output -> render.
struct SrMotionScale {
    float x = 1.0f, y = 1.0f;
};

inline SrMotionScale sr_motion_scale(uint32_t guideWidth, uint32_t guideHeight, float mvScaleX, float mvScaleY,
                                     uint32_t inputWidth, uint32_t inputHeight,
                                     uint32_t outputWidth, uint32_t outputHeight) noexcept
{
    SrMotionScale scale{};
    if (!guideWidth || !guideHeight || !inputWidth || !inputHeight || !outputWidth || !outputHeight ||
        !std::isfinite(mvScaleX) || !std::isfinite(mvScaleY))
        return scale;
    scale.x = mvScaleX * static_cast<float>(outputWidth) / static_cast<float>(guideWidth);
    scale.y = mvScaleY * static_cast<float>(outputHeight) / static_cast<float>(guideHeight);
    return scale;
}

struct SrFlowCounters {
    uint64_t frames = 0, ofaExec = 0, sharedHits = 0, sharedFallbacks = 0, localFallback = 0, sharedMisses = 0;

    void observe(SrFlowMode mode, bool executedOfa) noexcept
    {
        ++frames;
        if (executedOfa) ++ofaExec;
        switch (mode) {
        case SrFlowMode::UseShared: ++sharedHits; break;
        case SrFlowMode::ForbidLocal: ++sharedFallbacks; break;
        case SrFlowMode::LocalFallback: ++localFallback; break;
        }
    }
};

// Graphics-thread provenance query. Handles stay broker-owned; the worker
// leases the pair on its own device and signals the SR return fence.
struct ObsUpscaleFlowRequest {
    uint64_t sourceInstanceId = 0;
    uint64_t resourceGeneration = 0;
    uint64_t displayedTick = 0;
    uint64_t previousDisplayedTick = 0;
    uint64_t sharedMvToken = 0; // analysed submission serial when shared MV is usable
    SrFlowMode mode = SrFlowMode::LocalFallback;
    bool reset = false; // display provenance demands a fresh NGX history
    std::shared_ptr<nr::FlowBroker> broker;
    std::optional<nr::FramePairKey> pair;
};

// Per-frame SR ownership telemetry. `upscaled` plus `presentTick`/`capturedTick`
// tell the downstream FG tap that SR rendered at the output extent and that the
// presented frame trails the capture by one canonical tick.
struct ObsUpscaleDlssStats {
    uint64_t frames = 0;
    uint64_t ofaExec = 0;           // sr_ofa_exec_count: local D3D11 NVOF executions
    uint64_t sharedMvHits = 0;      // sr_shared_mv_hits
    uint64_t sharedMvFallbacks = 0; // sr_shared_mv_fallbacks: NR-owned without usable shared MV
    uint64_t sharedMvMisses = 0;    // displayed pair was Valid but the ring slot was gone
    uint64_t localFallback = 0;     // ConsumerFallback frames that ran local OFA
    uint64_t presentTick = 0, capturedTick = 0;
    bool upscaled = false, temporal = false, ngxActive = false;
};
#endif

ObsUpscaleDlss* obs_upscale_dlss_create() noexcept;
// May join the worker; call outside the OBS graphics lock.
void obs_upscale_dlss_destroy(ObsUpscaleDlss*) noexcept;
// Graphics context required. Invalidate output and reset temporal history on
// the next accepted capture (e.g. a cut, source replacement, bypass or HDR).
void obs_upscale_dlss_reset(ObsUpscaleDlss*) noexcept;
#if defined(_WIN32) && defined(OBS_NR_VFG_QA_ACTION)
bool obs_upscale_dlss_qa_legacy_pacing() noexcept;
#endif
// Graphics context required. Source is OBS SDR premultiplied RGBA/BGRA (or
// linear RGBA16F). Same-size input/output selects DLAA. Returns a borrowed
// linear premultiplied GS_RGBA16F texture for exactly present_tick, or
// nullptr so the caller can spatially render the same capture. A zero tick
// means no previous capture exists. Do not retain the pointer across draws.
// Model indices 0/1/2 select NGX presets K/M/L. A null or empty runtime
// path uses bin/64bit/dlss/nvngx_dlss.dll (relative to the OBS executable).
// `flow` is the displayed Shared-NVOF provenance for this capture; null keeps
// the consumer-local policy (NR absent, disabled, or unreachable).
gs_texture_t* obs_upscale_dlss_render(ObsUpscaleDlss*, gs_texture_t* sdr_source,
                                    uint64_t present_tick,
                                    uint32_t output_width, uint32_t output_height,
                                    uint32_t model = 1, const char* runtime_path = nullptr,
#ifdef _WIN32
                                    const ObsUpscaleFlowRequest* flow = nullptr
#else
                                    const void* flow = nullptr
#endif
                                    ) noexcept;
#ifdef _WIN32
// Thread-safe snapshot of the SR ownership counters for diagnostics and FG.
bool obs_upscale_dlss_stats(const ObsUpscaleDlss*, ObsUpscaleDlssStats*) noexcept;
#endif
