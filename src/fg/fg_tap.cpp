// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include "anr_dll_validator.h"
#include "fg_monitor_text.h"
#include "../nr_flow_broker.h"
#include "../obs_filter.h"
#include "../obs_upscale_filter.h"
#include "../obs_split_compare.h"
#include "../obs_quality_trace.h"
#include "sr_display.h"
#include <obs-module.h>
#include <graphics/vec4.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#ifdef OBS_NR_VFG_QA_ACTION
#include <charconv>
#include <string_view>
#endif

namespace nr::fg::vfg {
namespace {
using Microsoft::WRL::ComPtr;
constexpr char qualityKey[] = "vfg_quality_mode";
constexpr char legacyQualityKey[] = "vfg_quality";
constexpr char dedupModeKey[] = "vfg_dedup_mode";
constexpr char liveAnchorKey[] = "vfg_live_anchor";
constexpr char qualityWarningKey[] = "vfg_quality_warning";

struct CaptureScope {
    gs_texrender_t* buffer;
    bool begun = false;
    const bool srgb = gs_framebuffer_srgb_enabled(), linear = gs_get_linear_srgb();
    const gs_cull_mode cull = gs_get_cull_mode();
    explicit CaptureScope(gs_texrender_t* value) : buffer(value) { gs_blend_state_push(); }
    ~CaptureScope()
    {
        if (begun) gs_texrender_end(buffer);
        gs_enable_framebuffer_srgb(srgb);
        gs_set_linear_srgb(linear);
        gs_set_cull_mode(cull);
        gs_blend_state_pop();
    }
};

struct MonitorItem {
    std::string label;
    std::string deviceId;
};

struct Tap {
    explicit Tap(obs_source_t* s) : source(s) {}
    obs_source_t* source;
    std::mutex command;
    std::unique_ptr<Controller> controller;
    std::unique_ptr<ipc::Producer> producer;
    obs_upscale::Filter* upscale{};
    split::Setting splitSetting;
    split::Canvas splitCanvas;
    gs_texrender_t* capture{};
    obs_source_t* target{};
    uint32_t previousWidth{}, previousHeight{};
    uint32_t activeWidth{}, activeHeight{};
    bool activeSplit{};
    uint32_t multiplier{2};
    Quality quality{Quality::Medium};
    uint32_t dedupMode{static_cast<uint32_t>(DedupMode::Auto)};
    uint32_t liveAnchor{1};
    Backend backend{};
    PushFlowTier pushflowTier{};
    uint32_t gpuShotChange{}, experimentalAnrOptIn{}, hudLock{}, pacingRepair{}, adaptiveFg{}, adaptiveTargetFps{};
    std::wstring anrDllPath;
    uint32_t fpsOverlay{1};
    uint32_t lowLatency{1};
    uint32_t gpuPriority{1};
    bool hideFromCapture{false};
    std::string monitorId;
    uint64_t lastTick{};
    uint64_t rejectedSinceNs{};
    uint64_t retiredGeneration{};
    bool breakPair{};
    bool resizing{};
    bool stopping{true};
    // Upstream Shared NVOF discovery. The NR filter publishes its instance and
    // resource generation through nr_sampler_snapshot; the broker is keyed by
    // exactly that pair.
    bool nrPresent{};
    bool nrEnabled{};
    uint64_t nrInstance{}, nrGeneration{};
    uint32_t nrWidth{}, nrHeight{};
    uint32_t activeGuidesWidth{}, activeGuidesHeight{};
    uint64_t flowFrames{}, flowHits{};
    uint32_t flowLoggedOwner{0xffffffffu};
    uint64_t srPathGeneration{};
    int srPathChoice{-1};
    const nr::FlowBroker* flowCacheBroker{};
    uint64_t flowCacheGeneration{};
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4> flowMotion;
    std::array<Microsoft::WRL::ComPtr<ID3D11Fence>, 4> flowReady, flowReturn;
    std::array<HANDLE, 4> flowMotionHandle{}, flowReadyHandle{}, flowReturnHandle{};
    std::atomic_bool testCheckpointUsed{};
    std::atomic_bool hideSourceCanvas{false};
    std::atomic_bool vfgActive{false};
    std::atomic_bool detached{false};
};
std::atomic_uint64_t nextTap{0};

struct UpstreamNr {
    obs_source_t* filter{};
    uint64_t instance{}, generation{};
    uint32_t width{}, height{};
    int64_t admission{};
};

// Calls the NR filter's read-only sampler observation. The contract is the one
// nr-qa-host uses; an unrelated source answers false and is skipped.
bool sample_nr(obs_source_t* filter, UpstreamNr& out)
{
    if (!filter) return false;
    calldata_t data;
    calldata_init(&data);
    struct Free { calldata_t* value; ~Free() { calldata_free(value); } } free{&data};
    if (!proc_handler_call(obs_source_get_proc_handler(filter), "nr_sampler_snapshot", &data)) return false;
    out.filter = filter;
    out.instance = static_cast<uint64_t>(calldata_int(&data, "instance"));
    out.generation = static_cast<uint64_t>(calldata_int(&data, "resource_generation"));
    out.width = static_cast<uint32_t>(std::max<int64_t>(0, calldata_int(&data, "width")));
    out.height = static_cast<uint32_t>(std::max<int64_t>(0, calldata_int(&data, "height")));
    out.admission = calldata_int(&data, "admission");
    return out.instance != 0;
}

std::optional<UpstreamNr> upstream_nr(obs_source_t* parent, obs_source_t* self)
{
    if (!parent) return std::nullopt;
    struct Probe { obs_source_t* self; std::optional<UpstreamNr> found; } probe{self, std::nullopt};
    obs_source_enum_filters(parent, [](obs_source_t*, obs_source_t* child, void* opaque) {
        auto& p = *static_cast<Probe*>(opaque);
        if (p.found || child == p.self) return;
        UpstreamNr nr;
        if (sample_nr(child, nr)) p.found = nr;
    }, &probe);
    return probe.found;
}

// Reads the SR filter's read-only per-draw telemetry. The handler is registered
// by obs_upscale::create on the source that owns the integrated SR; a draw with
// no SR at all leaves `answered` false and the extent fallback in charge.
SrPresentSignal sample_sr(obs_source_t* source)
{
    SrPresentSignal signal;
    if (!source) return signal;
    calldata_t data;
    calldata_init(&data);
    struct Free { calldata_t* value; ~Free() { calldata_free(value); } } free{&data};
    if (!proc_handler_call(obs_source_get_proc_handler(source), "sr_flow_snapshot", &data)) return signal;
    signal.answered = true;
    calldata_get_bool(&data, "temporal", &signal.temporal);
    calldata_get_bool(&data, "upscaled", &signal.upscaled);
    signal.presentTick = static_cast<uint64_t>(std::max<int64_t>(0, calldata_int(&data, "present_tick")));
    signal.capturedTick = static_cast<uint64_t>(std::max<int64_t>(0, calldata_int(&data, "captured_tick")));
    return signal;
}

const char* flow_owner_name(nr::FlowOwnerPolicy owner)
{
    switch (owner) {
    case nr::FlowOwnerPolicy::NrOwned: return "nr_owned";
    case nr::FlowOwnerPolicy::ConsumerFallback: return "consumer_fallback";
    case nr::FlowOwnerPolicy::Unavailable: return "unavailable";
    }
    return "invalid";
}

const char* flow_provenance_name(nr::FlowProvenance value)
{
    switch (value) {
    case nr::FlowProvenance::Valid: return "valid";
    case nr::FlowProvenance::DedupReused: return "dedup_reused";
    case nr::FlowProvenance::HeldPrevious: return "held_previous";
    case nr::FlowProvenance::Reset: return "reset";
    case nr::FlowProvenance::Discontinuity: return "discontinuity";
    case nr::FlowProvenance::Unknown: return "unknown";
    }
    return "invalid";
}

void clear_flow_cache(Tap& tap) noexcept;

std::vector<MonitorItem> list_monitors()
{
    std::vector<MonitorItem> list;
    struct Arg { std::vector<MonitorItem>* list; };
    Arg arg{&list};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* a = reinterpret_cast<Arg*>(lp);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return TRUE;
        std::wstring devId;
        std::wstring friendly;
        for (DWORD i = 0;; ++i) {
            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            if (!EnumDisplayDevicesW(mi.szDevice, i, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) break;
            if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
            devId = dd.DeviceID;
            friendly = dd.DeviceString;
            break;
        }
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        const long w = mi.rcMonitor.right - mi.rcMonitor.left;
        const long h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        const bool primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        std::string label = wide_to_utf8(mi.szDevice) + " (" + std::to_string(w) + "x" + std::to_string(h) +
            " @ " + std::to_string(dm.dmDisplayFrequency) + "Hz" +
            (primary ? std::string(", ") + obs_module_text("VFG.Monitor.Primary") : "") + ")";
        a->list->push_back({std::move(label), wide_to_utf8(devId.c_str())});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&arg));
    return list;
}

void disconnect(Tap& tap, const char* cause, bool userStop = false) noexcept
{
    if (tap.flowFrames)
        blog(LOG_INFO, "[vfg-shared] summary frames=%llu shared=%llu hit_rate=%.4f cause=%s",
            static_cast<unsigned long long>(tap.flowFrames), static_cast<unsigned long long>(tap.flowHits),
            static_cast<double>(tap.flowHits) / static_cast<double>(tap.flowFrames), cause);
    tap.flowFrames = tap.flowHits = 0;
    clear_flow_cache(tap);
    tap.flowCacheBroker = nullptr;
    tap.flowCacheGeneration = 0;
    if (tap.controller) tap.controller->request_stop(userStop);
    if (tap.producer) {
        tap.retiredGeneration = tap.producer->generation();
        blog(LOG_INFO, "[vfg] original selected generation=%llu qpc_ns=%llu reason=%s",
            static_cast<unsigned long long>(tap.producer->generation()),
            static_cast<unsigned long long>(clock_ns()), cause);
        tap.producer.reset();
    }
    tap.activeWidth = 0;
    tap.activeHeight = 0;
    tap.resizing = false;
    tap.rejectedSinceNs = 0;
    tap.srPathChoice = -1;
    tap.stopping = true;
    tap.vfgActive.store(false, std::memory_order_release);
}

bool try_recover_peer_exit(Tap& tap, const char* reason, bool forceFaultOnTimeout = false)
{
    if (tap.stopping || tap.detached.load()) {
        disconnect(tap, reason);
        return false;
    }
    // Bracket only the graphics caller's enqueue. Logging, producer teardown
    // and asynchronous child reap/spawn are outside the measured interval.
    const auto startedNs = clock_ns();
    const bool scheduled = tap.controller->begin_async_fallback_relaunch(forceFaultOnTimeout);
    const auto elapsedNs = clock_ns() - startedNs;
    if (!scheduled) {
        disconnect(tap, reason);
        return false;
    }
    blog(LOG_INFO, "[vfg] recovery requested reason=%s enqueue_ns=%llu graphics_thread=%d",
        reason, nr::quality::id(elapsedNs), obs_in_task_thread(OBS_TASK_GRAPHICS) ? 1 : 0);
    if (tap.producer) tap.retiredGeneration = tap.producer->generation();
    tap.producer.reset();
    tap.resizing = false;
    tap.rejectedSinceNs = 0;
    tap.breakPair = true;
    return true;
}

struct TestWaitSpan {
    volatile int64_t waitStartNs;
    volatile int64_t waitEndNs;
};

// The isolated libobs fixture releases this bounded checkpoint after acting
// on its own presenter. It is never taken without both fixture events.
bool test_checkpoint(Tap& tap, const wchar_t* stage)
{
    if (tap.testCheckpointUsed) return false;
    wchar_t selected[24]{}, name[128]{};
    if (!GetEnvironmentVariableW(L"OBS_NR_TEST_TAP_STAGE", selected, ARRAYSIZE(selected)) ||
        wcscmp(selected, stage) ||
        GetEnvironmentVariableW(L"OBS_NR_TEST_TAP_EVENT", name, ARRAYSIZE(name)) >= ARRAYSIZE(name) ||
        !name[0]) return false;
    const auto observed = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!observed) return false;
    const auto resumed = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(name) + L".resume").c_str());
    if (resumed) {
        tap.testCheckpointUsed = true;
        const auto waitMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, (std::wstring(name) + L".wait").c_str());
        auto* waitView = waitMap ? static_cast<TestWaitSpan*>(MapViewOfFile(waitMap, FILE_MAP_WRITE, 0, 0, sizeof(TestWaitSpan))) : nullptr;
        SetEvent(observed);
        const auto waitStart = static_cast<int64_t>(clock_ns());
        WaitForSingleObject(resumed, 250);
        const auto waitEnd = static_cast<int64_t>(clock_ns());
        if (waitView) {
            InterlockedExchange64(&waitView->waitStartNs, waitStart);
            InterlockedExchange64(&waitView->waitEndNs, waitEnd);
            UnmapViewOfFile(waitView);
        }
        if (waitMap) CloseHandle(waitMap);
        CloseHandle(resumed);
    }
    CloseHandle(observed);
    return resumed != nullptr;
}

struct FlowPublish {
    ipc::FlowBundle bundle;
    Microsoft::WRL::ComPtr<ID3D11Fence> returnFence;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    uint64_t returnValue{};
};

// One publish's output extent and whether it starts a new pair.
struct FlowRequest {
    Extent output;
    bool reset;
};

void clear_flow_cache(Tap& tap) noexcept
{
    for (size_t i = 0; i < tap.flowMotion.size(); ++i) {
        tap.flowMotion[i].Reset();
        tap.flowReady[i].Reset();
        tap.flowReturn[i].Reset();
        tap.flowMotionHandle[i] = nullptr;
        tap.flowReadyHandle[i] = nullptr;
        tap.flowReturnHandle[i] = nullptr;
    }
}

// Shared NT handles are valid for one broker generation. Reopen only when the
// publisher replaced the handle; a failed open leaves the cache empty so the
// frame degrades to the original instead of reusing stale motion.
ID3D11Fence* open_fence(ID3D11Device5* device, HANDLE handle, ComPtr<ID3D11Fence>& cache, HANDLE& cached)
{
    if (cache && cached == handle) return cache.Get();
    cache.Reset();
    cached = nullptr;
    if (!handle || FAILED(device->OpenSharedFence(handle, IID_PPV_ARGS(&cache)))) return nullptr;
    cached = handle;
    return cache.Get();
}

ID3D11Texture2D* open_motion(ID3D11Device5* device, size_t slot, HANDLE handle,
    std::array<ComPtr<ID3D11Texture2D>, 4>& cache, std::array<HANDLE, 4>& cached)
{
    if (cache[slot] && cached[slot] == handle) return cache[slot].Get();
    cache[slot].Reset();
    cached[slot] = nullptr;
    if (!handle || FAILED(device->OpenSharedResource1(handle, IID_PPV_ARGS(&cache[slot])))) return nullptr;
    cached[slot] = handle;
    return cache[slot].Get();
}

ipc::FrameKey ipc_key(const nr::FlowColorKey& key)
{
    return {static_cast<uint32_t>(key.sourceInstanceId), static_cast<uint32_t>(key.resourceGeneration),
        key.submissionSerial, key.canonicalTick, key.historyEpoch, key.contentEpoch};
}

// Bridges the leased shared pair into the IPC slot. The D3D11 context waits on
// NR's ready fence before the producer's copy, so the copy never observes a
// half-written analysis texture; the caller signals the return fence afterwards.
std::optional<FlowPublish> bridge_flow(Tap& tap, const nr::ExportedFlow& lease, const FlowRequest& request)
{
    FlowPublish out;
    auto* device = static_cast<ID3D11Device*>(gs_get_device_obj());
    if (!device) return std::nullopt;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext4> context;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5)))) return std::nullopt;
    device->GetImmediateContext(&immediate);
    if (!immediate || FAILED(immediate.As(&context))) return std::nullopt;
    auto* returned = open_fence(device5.Get(), lease.returnFence, tap.flowReturn[lease.slot],
        tap.flowReturnHandle[lease.slot]);
    if (!returned) return std::nullopt;
    // Every acquired lease must be returned even when the bridge cannot use it.
    const auto release = [&] {
        (void)context->Signal(returned, lease.returnValue);
        context->Flush();
    };
    if (!lease.motion || !tap.activeGuidesWidth || !tap.activeGuidesHeight ||
        lease.guide.guideWidth != tap.activeGuidesWidth || lease.guide.guideHeight != tap.activeGuidesHeight) {
        release();
        return std::nullopt;
    }
    auto* motion = open_motion(device5.Get(), lease.slot, lease.motion, tap.flowMotion, tap.flowMotionHandle);
    auto* ready = open_fence(device5.Get(), lease.readyFence, tap.flowReady[lease.slot], tap.flowReadyHandle[lease.slot]);
    if (!motion || !ready || FAILED(context->Wait(ready, lease.readyValue))) {
        release();
        return std::nullopt;
    }
    const auto& pair = lease.pair;
    out.bundle.motion = motion;
    out.bundle.color = ipc_key(pair.currColorKey);
    out.bundle.pair.owner = ipc::FlowOwner::NrOwned;
    out.bundle.pair.provenance = ipc::FlowProvenance::Valid;
    out.bundle.pair.mvScaleX = static_cast<float>(request.output.width) / static_cast<float>(lease.guide.guideWidth);
    out.bundle.pair.mvScaleY = static_cast<float>(request.output.height) / static_cast<float>(lease.guide.guideHeight);
    out.bundle.pair.pairPrevTick = pair.prevColorKey.canonicalTick;
    out.bundle.pair.pairCurrTick = pair.currColorKey.canonicalTick;
    out.bundle.pair.shotCut = lease.guide.shotCut;
    out.bundle.pair.reset = request.reset || lease.guide.reset;
    out.bundle.pair.hasSharedMv = true;
    out.bundle.pair.prevColor = ipc_key(pair.prevColorKey);
    out.bundle.pair.currColor = ipc_key(pair.currColorKey);
    out.returnFence = tap.flowReturn[lease.slot];
    out.returnValue = lease.returnValue;
    out.context = context;
    return out;
}

// Single-owner producer policy. NR-owned frames never fall back to presenter
// analysis: a held/dropped/unmatched pair publishes original-only with reset.
// Consumer fallback is granted only when the upstream filter itself says NR is
// disabled; a missing broker record on an active NR chain is still NR-owned.
FlowPublish resolve_flow(Tap& tap, uint64_t displayTick, const FlowRequest& request)
{
    FlowPublish out;
    if (!tap.nrPresent || !tap.nrEnabled) {
        out.bundle.pair.owner = ipc::FlowOwner::ConsumerFallback;
        out.bundle.pair.provenance = ipc::FlowProvenance::Unknown;
        out.bundle.pair.reset = request.reset;
        return out;
    }
    auto broker = nr::find_flow_broker(tap.nrInstance, tap.nrGeneration);
    if (!broker) {
        out.bundle.pair.owner = ipc::FlowOwner::NrOwned;
        out.bundle.pair.provenance = ipc::FlowProvenance::Reset;
        out.bundle.pair.reset = true;
        return out;
    }
    if (tap.flowCacheBroker != broker.get() || tap.flowCacheGeneration != tap.nrGeneration) {
        clear_flow_cache(tap);
        tap.flowCacheBroker = broker.get();
        tap.flowCacheGeneration = tap.nrGeneration;
    }
    const auto displayed = broker->displayed(tap.nrGeneration, displayTick);
    if (!displayed) {
        out.bundle.pair.owner = ipc::FlowOwner::NrOwned;
        out.bundle.pair.provenance = ipc::FlowProvenance::Reset;
        out.bundle.pair.reset = true;
        return out;
    }
    if (displayed->owner == nr::FlowOwnerPolicy::ConsumerFallback) {
        out.bundle.pair.owner = ipc::FlowOwner::ConsumerFallback;
        out.bundle.pair.provenance = ipc::FlowProvenance::Unknown;
        out.bundle.pair.reset = request.reset;
        return out;
    }
    if (displayed->owner == nr::FlowOwnerPolicy::NrOwned && displayed->provenance == nr::FlowProvenance::Valid &&
        displayed->pair) {
        if (const auto lease = broker->find_displayed(tap.nrGeneration, displayTick, nr::FlowConsumer::Fg); lease) {
            if (auto bridged = bridge_flow(tap, *lease, request)) return *bridged;
        }
        out.bundle.pair.owner = ipc::FlowOwner::NrOwned;
        out.bundle.pair.provenance = ipc::FlowProvenance::HeldPrevious;
        out.bundle.pair.reset = true;
        return out;
    }
    out.bundle.pair.owner = static_cast<ipc::FlowOwner>(static_cast<uint32_t>(displayed->owner));
    out.bundle.pair.provenance = static_cast<ipc::FlowProvenance>(static_cast<uint32_t>(displayed->provenance));
    out.bundle.pair.reset = true;
    return out;
}

// The chosen delay path is logged once per NR generation (and whenever it
// changes) so a fallback is never silently in charge.
void log_sr_path(Tap& tap, const SrDisplayDecision& display, const SrPresentSignal& signal)
{
    const int choice = static_cast<int>(display.path);
    if (tap.srPathChoice == choice && tap.srPathGeneration == tap.nrGeneration) return;
    tap.srPathChoice = choice;
    tap.srPathGeneration = tap.nrGeneration;
    blog(LOG_INFO, "[vfg-sr] generation=%llu path=%s delayed=%d shift=%llu display_tick=%llu upscaled=%d temporal=%d present_ns=%llu captured_ns=%llu",
        static_cast<unsigned long long>(tap.nrGeneration),
        display.path == SrDelayPath::Snapshot ? "snapshot" : "extent_fallback",
        display.shift ? 1 : 0, static_cast<unsigned long long>(display.shift),
        static_cast<unsigned long long>(display.displayTick), signal.upscaled ? 1 : 0, signal.temporal ? 1 : 0,
        static_cast<unsigned long long>(signal.presentTick), static_cast<unsigned long long>(signal.capturedTick));
}

void draw_sdr(gs_texture_t* texture, Extent size, gs_blend_type destBlend)
{
    auto* effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    const auto prevLinear = gs_set_linear_srgb(true);
    const auto prevSrgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, destBlend);
    gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, size.width, size.height);
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(prevSrgb);
    gs_set_linear_srgb(prevLinear);
}

void render_effects(void* data)
{
    auto& tap = *static_cast<Tap*>(data);
    if (tap.upscale) obs_upscale::render(*tap.upscale);
    else obs_source_skip_video_filter(tap.source);
}

void draw_transparent(Extent size)
{
    if (!size.width || !size.height) return;
    auto* effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    const vec4 transparent{};
    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &transparent);
    // Keep the caller's blend mode: source-only targets can write zero, while
    // the scene's source-over blend keeps the layers already below this one.
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, size.width, size.height);
}

// Returns a displayable capture, if this draw also supplied the presenter.
gs_texture_t* publish(Tap& tap, obs_source_t* target, obs_source_t* parent, Extent output,
                      const split::Plan& compare, gs_texture_t*& before)
{
    if (!obs_in_task_thread(OBS_TASK_GRAPHICS)) return nullptr;
    std::unique_lock lock(tap.command, std::try_to_lock);
    if (!lock || !tap.controller) return nullptr;
    if (tap.stopping || tap.detached.load()) {
        disconnect(tap, "stopped_or_detached", true);
        return nullptr;
    }
    switch (tap.controller->poll_async_fallback_relaunch()) {
    case RelaunchState::InFlight: return nullptr;
    case RelaunchState::Failed: disconnect(tap, "relaunch_failed"); return nullptr;
    case RelaunchState::Idle:
    case RelaunchState::Ready: break;
    }
    test_checkpoint(tap, L"before_alive");
    if (!tap.controller->alive()) {
        try_recover_peer_exit(tap, "peer_exit");
        return nullptr;
    }
    const auto exit = tap.controller->exit_reason();
    if (exit == ExitReason::Watchdog || exit == ExitReason::UserClosed) return nullptr;
    if (tap.producer) test_checkpoint(tap, L"after_alive");
    const auto tick = obs_get_video_frame_time();
    if (tick == tap.lastTick) return nullptr;
    tap.lastTick = tick;
    const auto nr = upstream_nr(parent, tap.source);
    tap.nrPresent = nr.has_value();
    tap.nrEnabled = nr && nr->admission == static_cast<int64_t>(nr::sampler::Admission::Ready) &&
        obs_source_enabled(nr->filter);
    if (nr) {
        tap.nrInstance = nr->instance;
        tap.nrGeneration = nr->generation;
        tap.nrWidth = nr->width;
        tap.nrHeight = nr->height;
    } else {
        tap.nrInstance = tap.nrGeneration = 0;
        tap.nrWidth = tap.nrHeight = 0;
    }
    const uint32_t guideWidth = tap.nrPresent ? tap.nrWidth : 0;
    const uint32_t guideHeight = tap.nrPresent ? tap.nrHeight : 0;
    auto* kind = obs_source_get_unversioned_id(parent);
    const gs_color_space wanted[]{GS_CS_SRGB};
    const uint32_t w = output.width, h = output.height;
    if (!kind || strcmp(kind, "monitor_capture") == 0 ||
        gs_get_device_type() != GS_DEVICE_DIRECT3D_11 || obs_source_get_color_space(target, 1, wanted) != GS_CS_SRGB ||
        !output.width || !output.height ||
        w < 320 || h < 180 || w > 7680 || h > 4320) {
        disconnect(tap, "source_or_size_changed_or_unsupported");
        return nullptr;
    }
    try {
        if (tap.producer && !tap.resizing &&
            (tap.activeWidth != w || tap.activeHeight != h || tap.activeSplit != bool(compare) ||
                tap.activeGuidesWidth != guideWidth || tap.activeGuidesHeight != guideHeight)) {
            if (!tap.controller->request_resize()) {
                if (!tap.controller->alive() || tap.controller->is_fault_or_watchdog_exit())
                    try_recover_peer_exit(tap, "resize_request_peer_fault");
                else disconnect(tap, "resize_request");
                return nullptr;
            }
            tap.resizing = true;
            blog(LOG_INFO, "[vfg] resize requested old=%ux%u new=%ux%u generation=%llu",
                tap.activeWidth, tap.activeHeight, w, h, nr::quality::id(tap.producer->generation()));
        }
        if (tap.resizing) {
            if (!tap.controller->resize_drained()) {
                if (tap.controller->is_fault_or_watchdog_exit())
                    try_recover_peer_exit(tap, "resize_drain_peer_fault");
                return nullptr;
            }
            const auto retired = tap.producer->retire();
            if (retired == ipc::Status::PeerBusy) return nullptr;
            if (retired != ipc::Status::Ok) {
                if (retired == ipc::Status::PeerDead || retired == ipc::Status::Closed)
                    try_recover_peer_exit(tap, "resize_retire_peer_exit");
                else disconnect(tap, "resize_retire");
                return nullptr;
            }
            tap.producer.reset();
            tap.resizing = false;
            tap.breakPair = true;
        }
        ipc::Shape shape{w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
        shape.splitCompare = bool(compare);
        shape.guides = {guideWidth, guideHeight};
        if (!tap.producer) {
            auto* device = static_cast<ID3D11Device*>(gs_get_device_obj());
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
                FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) {
                disconnect(tap, "adapter_query");
                return nullptr;
            }
            const auto key = LR"(Local\obs-nr-vfg.)" + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++nextTap);
            tap.producer = std::make_unique<ipc::Producer>(device, key, shape);
            ipc::Offer offer;
            try {
                test_checkpoint(tap, L"before_connect");
                auto peer = tap.controller->peer();
                if (test_checkpoint(tap, L"bad_peer")) ++peer.creation;
                offer = tap.producer->connect(peer);
            } catch (const ipc::Error&) {
                if (tap.controller->alive() && !tap.controller->is_fault_or_watchdog_exit()) throw;
                try_recover_peer_exit(tap, "connect_peer_dead");
                return nullptr;
            }
            tap.target = target;
            tap.activeWidth = w;
            tap.activeHeight = h;
            tap.activeSplit = shape.splitCompare;
            tap.activeGuidesWidth = shape.guides.width;
            tap.activeGuidesHeight = shape.guides.height;
            if (!tap.controller->offer(offer, shape, desc.AdapterLuid)) {
                if (!tap.controller->alive() || tap.controller->is_fault_or_watchdog_exit())
                    try_recover_peer_exit(tap, "offer_peer_fault");
                else disconnect(tap, "offer_rejected");
                return nullptr;
            }
            blog(LOG_INFO, "[vfg] offered source_uuid=%s tap_uuid=%s size=%ux%u generation=%llu qpc_ns=%llu",
                obs_source_get_uuid(parent), obs_source_get_uuid(tap.source), w, h,
                static_cast<unsigned long long>(offer.generation), static_cast<unsigned long long>(clock_ns()));
        }
        const auto status = tap.producer->status();
        if (status == ipc::Status::Pending) return nullptr;
        if (status == ipc::Status::Rejected) {
            if (tap.controller->can_recover() && tap.controller->alive() &&
                tap.controller->exit_reason() == ExitReason::None) {
                const auto now = clock_ns();
                if (!tap.rejectedSinceNs) tap.rejectedSinceNs = now;
                if (now - tap.rejectedSinceNs < 500'000'000) return nullptr;
                try_recover_peer_exit(tap, "import_rejected_timeout", true);
            } else try_recover_peer_exit(tap, "import_rejected");
            return nullptr;
        }
        if (status == ipc::Status::PeerDead || status == ipc::Status::Closed) {
            try_recover_peer_exit(tap, "transport_peer_closed");
            return nullptr;
        }
        tap.rejectedSinceNs = 0;
        if (status != ipc::Status::Ok) { disconnect(tap, "transport_status"); return nullptr; }
        if (tap.target != target || tap.previousWidth != output.width || tap.previousHeight != output.height) {
            tap.breakPair = true;
            tap.target = target;
            tap.previousWidth = output.width;
            tap.previousHeight = output.height;
        }
        if (!tap.capture)
            tap.capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (!tap.capture) { disconnect(tap, "texture_allocation"); return nullptr; }
        const auto captureQpc = clock_ns();
        if (compare)
            before = tap.splitCanvas.original(compare, output, GS_CS_SRGB, true);
        gs_texrender_reset(tap.capture);
        {
            CaptureScope scope(tap.capture);
            gs_enable_framebuffer_srgb(true);
            scope.begun = gs_texrender_begin_with_color_space(tap.capture, output.width, output.height, GS_CS_SRGB);
            if (!scope.begun) { disconnect(tap, "capture_begin"); return nullptr; }
            vec4 clear{};
            gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
            gs_ortho(0, float(output.width), 0, float(output.height), -100, 100);
            gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
            if (compare) {
                const split::Suppress suppress(compare);
                render_effects(&tap);
            } else {
                render_effects(&tap);
            }
        }
        auto* texture = gs_texrender_get_texture(tap.capture);
        if (compare && !before) {
            disconnect(tap, "split_original_capture");
            return texture;
        }
        const auto arrival = clock_ns();
        // The SR renders inside this capture, so its snapshot is the authoritative
        // colour delay: DLSS presents the previous capture even at 1:1, while a
        // bilinear resize changes extent without ever delaying. The extent
        // comparison is only the fallback when no SR telemetry answers at all.
        const Extent inputExtent{obs_source_get_base_width(target), obs_source_get_base_height(target)};
        const SrDraw draw{tick, obs_get_frame_interval_ns(),
            output.width != inputExtent.width || output.height != inputExtent.height};
        const auto srSignal = sample_sr(tap.source);
        const auto display = sr_display_decision(draw, srSignal);
        log_sr_path(tap, display, srSignal);
        auto flow = resolve_flow(tap, display.displayTick, {output, tap.breakPair});
        if (tap.nrPresent && tap.nrEnabled) {
            ++tap.flowFrames;
            if (flow.bundle.pair.hasSharedMv) ++tap.flowHits;
            const auto owner = static_cast<uint32_t>(flow.bundle.pair.owner);
            if (owner != tap.flowLoggedOwner) {
                tap.flowLoggedOwner = owner;
                blog(LOG_INFO, "[vfg-shared] generation=%llu owner=%s provenance=%s guides=%ux%u output=%ux%u display_tick=%llu shared=%d",
                    static_cast<unsigned long long>(tap.nrGeneration), flow_owner_name(static_cast<nr::FlowOwnerPolicy>(owner)),
                    flow_provenance_name(static_cast<nr::FlowProvenance>(static_cast<uint32_t>(flow.bundle.pair.provenance))),
                    guideWidth, guideHeight, w, h, static_cast<unsigned long long>(display.displayTick),
                    flow.bundle.pair.hasSharedMv ? 1 : 0);
            }
        } else if (tap.flowLoggedOwner != static_cast<uint32_t>(ipc::FlowOwner::ConsumerFallback)) {
            tap.flowLoggedOwner = static_cast<uint32_t>(ipc::FlowOwner::ConsumerFallback);
            blog(LOG_INFO, "[vfg-shared] owner=consumer_fallback provenance=unknown reason=no_upstream_nr output=%ux%u display_tick=%llu",
                w, h, static_cast<unsigned long long>(display.displayTick));
        }
        const auto result = tap.producer->publish(texture ? static_cast<ID3D11Texture2D*>(gs_texture_get_obj(texture)) : nullptr,
            captureQpc, arrival, tap.breakPair, tick,
            before ? static_cast<ID3D11Texture2D*>(gs_texture_get_obj(before)) : nullptr, compare.percent, compare.swap,
            flow.bundle);
        if (flow.returnFence && flow.context) {
            // The producer copy is ordered on the immediate context before this
            // signal, so the broker can reuse the ring slot after GPU completion.
            if (FAILED(flow.context->Signal(flow.returnFence.Get(), flow.returnValue)))
                blog(LOG_WARNING, "[vfg-shared] return fence signal failed");
            flow.context->Flush();
        }
        if (nr::quality::trace_frames())
            blog(LOG_INFO, "[quality] stage=vfg_tap obs_tick_ns=%llu source=%s generation=%llu capture_qpc_ns=%llu arrival_qpc_ns=%llu status=%d",
                nr::quality::id(tick), obs_source_get_uuid(parent), nr::quality::id(tap.producer->generation()),
                nr::quality::id(captureQpc), nr::quality::id(arrival), static_cast<int>(result));
        if (result == ipc::Status::Ok)
            tap.breakPair = false;
        if (result == ipc::Status::PeerDead || result == ipc::Status::Closed ||
            (result != ipc::Status::Ok && result != ipc::Status::Dropped &&
                (!tap.controller->alive() || tap.controller->is_fault_or_watchdog_exit())))
            try_recover_peer_exit(tap, "publish_peer_closed");
        else if (result != ipc::Status::Ok && result != ipc::Status::Dropped) {
            blog(LOG_WARNING, "[vfg] transport publish failed status=%d", static_cast<int>(result));
            disconnect(tap, "publish_fault");
        }
        return texture;
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "[vfg] OBS tap fault: %s", e.what());
        disconnect(tap, "tap_exception");
        return nullptr;
    }
}

void render(void* data, gs_effect_t*)
{
    auto& tap = *static_cast<Tap*>(data);
    if (split::capturing_original(tap.source))
        return;
    auto* target = obs_filter_get_target(tap.source);
    auto* parent = obs_filter_get_parent(tap.source);
    if (!target || !parent)
        return;
    if (obs_in_task_thread(OBS_TASK_GRAPHICS)) test_checkpoint(tap, L"before_extent");
    const auto size = tap.upscale ? obs_upscale::dimensions(*tap.upscale) :
        Extent{obs_source_get_base_width(target), obs_source_get_base_height(target)};
    const auto compare = split::plan(tap.source);
    gs_texture_t* before{};
    auto* texture = publish(tap, target, parent, size, compare, before);
    if (should_hide_source_canvas(tap.hideSourceCanvas.load(std::memory_order_acquire),
            tap.vfgActive.load(std::memory_order_acquire))) {
        draw_transparent(size);
        return;
    }
    if (!texture || gs_get_color_space() != GS_CS_SRGB) {
        if (!compare || !tap.splitCanvas.render(compare, size, render_effects, &tap))
            render_effects(&tap);
        return;
    }
    if (compare && tap.splitCanvas.draw(before, texture, size, compare.percent, compare.swap))
        return;
    draw_sdr(texture, size, GS_BLEND_INVSRCALPHA);
}

bool start(Tap& tap, bool off)
{
    if (obs_in_task_thread(OBS_TASK_GRAPHICS)) return false;
    std::lock_guard lock(tap.command);
    if (tap.detached.load() || tap.producer || (tap.controller && !tap.controller->exited()) ||
        (tap.retiredGeneration && ipc::Producer::generation_retained(tap.retiredGeneration))) return false;
    if (tap.backend == Backend::AnrExperimental && !tap.experimentalAnrOptIn) {
        blog(LOG_WARNING, "[vfg] presenter launch refused: %s", obs_module_text("VFG.Status.AnrOptInRequired"));
        return false;
    }
    // Settle any exited Controller's observer/marker cleanup before creating the next session.
    tap.controller.reset();
    try {
        LaunchOptions opts{};
        opts.multiplier = off ? 1u : std::clamp<uint32_t>(tap.multiplier, 2, max_multiplier);
        opts.quality = tap.quality;
        opts.dedupMode = tap.dedupMode;
        opts.liveAnchor = tap.liveAnchor;
        opts.transportIntervalNs = obs_get_frame_interval_ns();
        opts.monitorId = utf8_to_wide(tap.monitorId);
        opts.fpsOverlay = std::min<uint32_t>(tap.fpsOverlay, 2);
        opts.lowLatency = tap.lowLatency ? 1u : 0u;
        opts.gpuPriority = tap.gpuPriority ? 1u : 0u;
        opts.hideFromCapture = tap.hideFromCapture ? 1u : 0u;
        opts.backend = tap.backend;
        opts.pushflowTier = tap.pushflowTier;
        opts.gpuShotChange = tap.gpuShotChange;
        opts.experimentalAnrOptIn = tap.experimentalAnrOptIn;
        opts.anrDllPath = tap.anrDllPath;
        opts.hudLock = tap.hudLock;
        opts.pacingRepair = tap.pacingRepair;
        opts.adaptiveFg = tap.adaptiveFg;
        opts.adaptiveTargetFps = tap.adaptiveTargetFps;
        tap.controller = std::make_unique<Controller>(opts);
        tap.testCheckpointUsed = false;
        tap.stopping = false;
        tap.vfgActive.store(true, std::memory_order_release);
        blog(LOG_INFO, "[vfg] open request mode=%ux quality=%u dedup_mode=%u live_anchor=%u transport_interval_ns=%llu qpc_ns=%llu",
            opts.multiplier, static_cast<unsigned>(opts.quality), opts.dedupMode, opts.liveAnchor,
            static_cast<unsigned long long>(opts.transportIntervalNs), static_cast<unsigned long long>(clock_ns()));
        return true;
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "[vfg] presenter launch failed: %s", e.what());
        return false;
    }
}

void stop(void* data)
{
    auto& tap = *static_cast<Tap*>(data);
    obs_weak_source_t* queued = nullptr;
    {
        std::lock_guard lock(tap.command);
        if (tap.controller) tap.controller->stop(true);
        if (!tap.stopping && tap.producer) queued = obs_source_get_weak_source(tap.source);
        tap.stopping = true;
        tap.vfgActive.store(false, std::memory_order_release);
    }
    if (queued) obs_queue_task(OBS_TASK_GRAPHICS, [](void* opaque) {
        auto* weak = static_cast<obs_weak_source_t*>(opaque);
        const std::unique_ptr<obs_source_t, decltype(&obs_source_release)> source(
            obs_weak_source_get_source(weak), &obs_source_release);
        obs_weak_source_release(weak);
        if (!source) return;
        auto* state = static_cast<Tap*>(obs_obj_get_data(source.get()));
        if (!state) return;
        obs_enter_graphics();
        {
            std::lock_guard lock(state->command);
            if (state->stopping && state->producer) disconnect(*state, "close_or_filter_removed", true);
        }
        obs_leave_graphics();
    }, queued, false);
}

std::wstring anr_path_from_settings(obs_data_t* settings)
{
    auto path = utf8_to_wide(obs_data_get_string(settings, "vfg_anr_dll_path"));
    // OBS's Windows picker returns drive paths with forward slashes.
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'/' || path[2] == L'\\') &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        std::ranges::replace(path, L'/', L'\\');
    return path;
}

void apply_settings(Tap& tap, obs_data_t* settings)
{
    const auto mult = static_cast<uint32_t>(obs_data_get_int(settings, "vfg_multiplier"));
    // Older scenes saved the opposite numbers for the Fast and Best UI choices.
    if (obs_data_has_user_value(settings, legacyQualityKey)) {
        if (!obs_data_has_user_value(settings, qualityKey)) {
            const auto legacy = obs_data_get_int(settings, legacyQualityKey);
            const auto migrated = legacy == 0 ? Quality::High : legacy == 2 ? Quality::Low : Quality::Medium;
            obs_data_set_int(settings, qualityKey, static_cast<long long>(migrated));
        }
        obs_data_unset_user_value(settings, legacyQualityKey);
    }
    const auto quality = decode_quality(obs_data_get_int(settings, qualityKey));
    const auto dedupRaw = obs_data_get_int(settings, dedupModeKey);
    const bool dedupValid = is_valid_dedup_mode(static_cast<uint32_t>(dedupRaw));
    // An out-of-range saved value (for example 99) must never reach the session header.
    if (!dedupValid) obs_data_set_int(settings, dedupModeKey, static_cast<long long>(DedupMode::OffExact));
    const auto dedup = dedupValid ? static_cast<uint32_t>(dedupRaw) : static_cast<uint32_t>(DedupMode::OffExact);
    const uint32_t liveAnchor = obs_data_get_bool(settings, liveAnchorKey) ? 1u : 0u;
    const auto hud = static_cast<uint32_t>(std::clamp<long long>(obs_data_get_int(settings, "vfg_fps_overlay"), 0, 2));
    const uint32_t ll = obs_data_get_bool(settings, "vfg_low_latency") ? 1u : 0u;
    const uint32_t gp = obs_data_get_bool(settings, "vfg_gpu_priority") ? 1u : 0u;
    const bool hideFromCapture = obs_data_get_bool(settings, "vfg_hide_from_capture");
    const bool hideSourceCanvas = obs_data_get_bool(settings, "vfg_hide_source_canvas");
    const char* mon = obs_data_get_string(settings, "vfg_monitor");
    const bool enabled = obs_data_get_bool(settings, "vfg_upscale");
    const bool compare = obs_data_get_bool(settings, "vfg_upscale_compare");
    std::lock_guard lock(tap.command);
    tap.splitSetting.update(settings);
    if (obs_upscale::update(tap.upscale, settings, enabled && !compare, enabled))
        tap.breakPair = true;
    const auto previousDedup = tap.dedupMode;
    const auto previousLiveAnchor = tap.liveAnchor;
    tap.multiplier = std::clamp<uint32_t>(mult ? mult : 2, 2, max_multiplier);
    tap.quality = quality;
    tap.dedupMode = dedup;
    tap.liveAnchor = liveAnchor;
    tap.fpsOverlay = hud;
    tap.lowLatency = ll;
    tap.gpuPriority = gp;
    tap.hideFromCapture = hideFromCapture;
    switch (obs_data_get_int(settings, "vfg_backend")) {
    case 1: tap.backend = Backend::PushFlow; break;
    case 2: tap.backend = Backend::AnrExperimental; break;
    default: tap.backend = Backend::NativeVfg; break;
    }
    tap.pushflowTier = obs_data_get_int(settings, "vfg_pushflow_tier") == 1 ? PushFlowTier::Quality : PushFlowTier::Fast;
    tap.experimentalAnrOptIn = obs_data_get_bool(settings, "vfg_experimental_anr");
    tap.anrDllPath = anr_path_from_settings(settings);
    tap.gpuShotChange = obs_data_get_bool(settings, "vfg_gpu_shot_change");
    tap.hudLock = obs_data_get_bool(settings, "vfg_hud_lock");
    tap.pacingRepair = obs_data_get_bool(settings, "vfg_pacing_repair");
    tap.adaptiveFg = obs_data_get_bool(settings, "vfg_adaptive_fg");
    tap.adaptiveTargetFps = static_cast<uint32_t>(std::clamp(obs_data_get_int(settings, "vfg_adaptive_target_fps"), 0LL, static_cast<long long>(LONG_MAX)));
    tap.hideSourceCanvas.store(hideSourceCanvas, std::memory_order_release);
    tap.monitorId = mon ? mon : "";
    if (tap.controller) {
        tap.controller->set_fps_overlay(tap.fpsOverlay);
        tap.controller->set_low_latency(tap.lowLatency);
        tap.controller->set_hide_from_capture(tap.hideFromCapture ? 1u : 0u);
        tap.controller->set_hud_lock(tap.hudLock);
        tap.controller->set_pacing_repair(tap.pacingRepair);
        tap.controller->set_adaptive_fg(tap.adaptiveFg);
        tap.controller->set_adaptive_target_fps(tap.adaptiveTargetFps);
        tap.controller->set_dedup_mode(tap.dedupMode);
        tap.controller->set_live_anchor(tap.liveAnchor);
        if (tap.dedupMode != previousDedup || tap.liveAnchor != previousLiveAnchor)
            blog(LOG_INFO, "[vfg] live settings applied dedup_mode=%u live_anchor=%u", tap.dedupMode, tap.liveAnchor);
    }
}

#ifdef OBS_NR_VFG_QA_ACTION
constexpr char qaKey[] = "vfg_qa_request_v1";
constexpr std::size_t qaTokenSize = 32;
struct QaAction {
    char token[qaTokenSize + 1]{};
    std::atomic_bool pending{false};
    std::atomic_ullong lastSequence{0};
    bool armed{};
};
QaAction qa;

void init_qa_action()
{
    const auto n = GetEnvironmentVariableA("OBS_NR_VFG_QA_ARM", qa.token, ARRAYSIZE(qa.token));
    qa.armed = n == qaTokenSize && std::string_view(qa.token, qaTokenSize).find_first_not_of("0123456789abcdef") == std::string_view::npos;
    blog(LOG_INFO, "[vfg-qa] gate=%s", qa.armed ? "armed" : "unarmed");
}

enum class QaVerb { Open2x, Close };
struct QaRequest {
    obs_weak_source_t* source;
    unsigned long long sequence;
    QaVerb verb;
    ~QaRequest() { obs_weak_source_release(source); }
};

void qa_ack(unsigned long long sequence, const char* outcome)
{
    blog(LOG_INFO, "[vfg-qa] seq=%llu outcome=%s qpc_ns=%llu", sequence, outcome,
        static_cast<unsigned long long>(clock_ns()));
}

void qa_dispatch(void* opaque)
{
    struct ReleasePending { ~ReleasePending() { qa.pending.store(false); } } pending;
    const std::unique_ptr<QaRequest> request(static_cast<QaRequest*>(opaque));
    if (!obs_in_task_thread(OBS_TASK_UI) || obs_in_task_thread(OBS_TASK_GRAPHICS)) {
        qa_ack(request->sequence, "wrong_thread");
        return;
    }
    const std::unique_ptr<obs_source_t, decltype(&obs_source_release)> source(
        obs_weak_source_get_source(request->source), &obs_source_release);
    auto* tap = source ? static_cast<Tap*>(obs_obj_get_data(source.get())) : nullptr;
    if (!tap) {
        qa_ack(request->sequence, "source_gone");
        return;
    }
    if (request->verb == QaVerb::Open2x) {
        qa_ack(request->sequence, start(*tap, false) ? "opened" : "rejected");
        return;
    }
    stop(tap);
    qa_ack(request->sequence, "closed");
}

void qa_update(void* opaque, obs_data_t* settings)
{
    if (!opaque || !settings) return;
    apply_settings(*static_cast<Tap*>(opaque), settings);
    if (!qa.armed) return;
    const char* raw = obs_data_get_string(settings, qaKey);
    if (!raw || !*raw) return;
    const std::string value(raw);
    obs_data_unset_user_value(settings, qaKey);
    if (value.size() <= qaTokenSize + 1 || value[qaTokenSize] != ':' ||
        std::string_view(value.data(), qaTokenSize) != std::string_view(qa.token, qaTokenSize)) {
        qa_ack(0, "invalid_arm");
        return;
    }
    const auto rest = std::string_view(value).substr(qaTokenSize + 1);
    const auto colon = rest.find(':');
    if (colon == std::string_view::npos) { qa_ack(0, "invalid_request"); return; }
    unsigned long long sequence = 0;
    const auto limit = rest.data() + colon;
    const auto parsed = std::from_chars(rest.data(), limit, sequence);
    const auto action = rest.substr(colon + 1);
    if (parsed.ec != std::errc{} || parsed.ptr != limit || !sequence || (action != "open2x" && action != "close")) {
        qa_ack(0, "invalid_request");
        return;
    }
    auto last = qa.lastSequence.load();
    do {
        if (sequence <= last) { qa_ack(sequence, "duplicate_or_stale"); return; }
    } while (!qa.lastSequence.compare_exchange_strong(last, sequence));
    if (qa.pending.exchange(true)) { qa_ack(sequence, "busy"); return; }
    auto& tap = *static_cast<Tap*>(opaque);
    auto* weak = obs_source_get_weak_source(tap.source);
    auto* request = new (std::nothrow) QaRequest{weak, sequence, action == "open2x" ? QaVerb::Open2x : QaVerb::Close};
    if (!request) {
        obs_weak_source_release(weak);
        qa.pending.store(false);
        qa_ack(sequence, "allocation_failed");
        return;
    }
    obs_queue_task(OBS_TASK_UI, qa_dispatch, request, false);
}
#endif

bool refresh_quality_warning(obs_properties_t* properties, obs_property_t*, obs_data_t* settings)
{
    obs_property_set_visible(obs_properties_get(properties, qualityWarningKey),
        obs_data_get_int(settings, qualityKey) == 0);
    return true;
}

bool refresh_backend_properties(obs_properties_t* properties, obs_property_t*, obs_data_t* settings)
{
    const auto backend = obs_data_get_int(settings, "vfg_backend");
    const bool experimental = backend == static_cast<long long>(Backend::AnrExperimental);
    const bool pushflow = backend == static_cast<long long>(Backend::PushFlow);
    const bool optedIn = obs_data_get_bool(settings, "vfg_experimental_anr");
    obs_property_set_visible(obs_properties_get(properties, "vfg_pushflow_tier"),
        pushflow);
    obs_property_set_visible(obs_properties_get(properties, "vfg_experimental_anr"), experimental);
    obs_property_set_visible(obs_properties_get(properties, "vfg_anr_warning"), experimental);
    obs_property_set_visible(obs_properties_get(properties, "vfg_anr_dll_path"),
        experimental && optedIn);
    auto* status = obs_properties_get(properties, "vfg_status");
    const char* key;
    if (pushflow) {
        const bool quality = obs_data_get_int(settings, "vfg_pushflow_tier") == 1;
        const auto tier = quality ? PushFlowTier::Quality : PushFlowTier::Fast;
        const bool found = !Controller::find_pushflow_dir(tier).empty();
        key = quality ? (found ? "VFG.Status.PushFlow.QualityFound" : "VFG.Status.PushFlow.QualityMissing")
                      : (found ? "VFG.Status.PushFlow.FastFound" : "VFG.Status.PushFlow.FastMissing");
    } else if (experimental) {
        if (!optedIn)
            key = "VFG.Status.AnrOptInRequired";
        else if (const auto path = anr_path_from_settings(settings); path.empty())
            key = "VFG.Status.ANR.Select";
        else {
            const auto result = anr::validate_addon(path, optedIn);
            if (result.status == anr::ValidationStatus::Accepted)
                key = "VFG.Status.ANR.Validated";
            else if (result.status == anr::ValidationStatus::IncompleteAddonMissingCompanions)
                key = "VFG.Status.ANR.Incomplete";
            else
                key = "VFG.Status.ANR.Invalid";
        }
    } else {
        key = Controller::find_runtime_dir().empty() ? "VFG.Status.MissingRuntime" : "VFG.Status.Ready";
    }
    obs_property_set_description(status, obs_module_text(key));
    return true;
}

obs_properties_t* properties(void* opaque)
{
    auto* p = obs_properties_create();
    auto* coreGrp = obs_properties_create();
    auto* backend = obs_properties_add_list(coreGrp, "vfg_backend", obs_module_text("VFG.Backend"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(backend, obs_module_text("VFG.Backend.Native"), static_cast<long long>(Backend::NativeVfg));
    obs_property_list_add_int(backend, obs_module_text("VFG.Backend.PushFlow"), static_cast<long long>(Backend::PushFlow));
    obs_property_list_add_int(backend, obs_module_text("VFG.Backend.ANR"), static_cast<long long>(Backend::AnrExperimental));
    obs_property_set_long_description(backend, obs_module_text("VFG.Hint.Reopen"));
    obs_property_set_modified_callback(backend, refresh_backend_properties);
    auto* tier = obs_properties_add_list(coreGrp, "vfg_pushflow_tier", obs_module_text("VFG.PushFlowTier"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(tier, obs_module_text("VFG.PushFlowTier.Fast"), static_cast<long long>(PushFlowTier::Fast));
    obs_property_list_add_int(tier, obs_module_text("VFG.PushFlowTier.Quality"), static_cast<long long>(PushFlowTier::Quality));
    obs_property_set_long_description(tier, obs_module_text("VFG.Hint.Reopen"));
    obs_property_set_modified_callback(tier, refresh_backend_properties);
    auto* warning = obs_properties_add_text(coreGrp, "vfg_anr_warning", obs_module_text("VFG.AnrWarning"), OBS_TEXT_INFO);
    obs_property_set_enabled(warning, false);
    auto* optIn = obs_properties_add_bool(coreGrp, "vfg_experimental_anr", obs_module_text("VFG.ExperimentalANR"));
    obs_property_set_long_description(optIn, obs_module_text("VFG.Hint.Reopen"));
    obs_property_set_modified_callback(optIn, refresh_backend_properties);
    auto* path = obs_properties_add_path(coreGrp, "vfg_anr_dll_path", obs_module_text("VFG.AnrDllPath"),
        OBS_PATH_FILE, "nvngx_dlssg.dll (nvngx_dlssg.dll)", nullptr);
    obs_property_set_long_description(path, obs_module_text("VFG.Hint.Reopen"));
    obs_property_set_modified_callback(path, refresh_backend_properties);

    auto* mult = obs_properties_add_list(coreGrp, "vfg_multiplier", obs_module_text("VFG.Multiplier"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mult, "2x (120fps)", 2);
    obs_property_list_add_int(mult, "3x (180fps)", 3);
    obs_property_list_add_int(mult, "4x (240fps)", 4);
    obs_property_set_long_description(mult, obs_module_text("VFG.Hint.Multiplier"));

    auto* qual = obs_properties_add_list(coreGrp, qualityKey, obs_module_text("VFG.Quality"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(qual, obs_module_text("VFG.Quality.Fast"), static_cast<long long>(Quality::Low));
    obs_property_list_add_int(qual, obs_module_text("VFG.Quality.Balanced"), static_cast<long long>(Quality::Medium));
    obs_property_list_add_int(qual, obs_module_text("VFG.Quality.Best"), static_cast<long long>(Quality::High));
    obs_property_set_long_description(qual, obs_module_text("VFG.Hint.Reopen"));
    auto* qualityWarning = obs_properties_add_text(coreGrp, qualityWarningKey,
        obs_module_text("VFG.QualityWarning"), OBS_TEXT_INFO);
    obs_property_set_enabled(qualityWarning, false);
    bool fastQuality = false;
    if (opaque) {
        auto* tap = static_cast<Tap*>(opaque);
        std::lock_guard lock(tap->command);
        fastQuality = tap->quality == Quality::Low;
    }
    obs_property_set_visible(qualityWarning, fastQuality);
    obs_property_set_modified_callback(qual, refresh_quality_warning);

    auto* dedup = obs_properties_add_list(coreGrp, dedupModeKey, obs_module_text("VFG.DedupMode"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(dedup, obs_module_text("VFG.DedupMode.Off"), static_cast<long long>(DedupMode::OffExact));
    obs_property_list_add_int(dedup, obs_module_text("VFG.DedupMode.Auto"), static_cast<long long>(DedupMode::Auto));
    obs_property_list_add_int(dedup, obs_module_text("VFG.DedupMode.Adaptive"),
        static_cast<long long>(DedupMode::Adaptive60To30));
    obs_property_list_add_int(dedup, obs_module_text("VFG.DedupMode.Fixed"),
        static_cast<long long>(DedupMode::Fixed60To30));
    obs_property_set_long_description(dedup, obs_module_text("VFG.Hint.DedupMode"));

    auto* liveAnchor = obs_properties_add_bool(coreGrp, liveAnchorKey, obs_module_text("VFG.LiveAnchor"));
    obs_property_set_long_description(liveAnchor, obs_module_text("VFG.Hint.LiveAnchor"));

    auto* mon = obs_properties_add_list(coreGrp, "vfg_monitor", obs_module_text("VFG.Monitor"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(mon, obs_module_text("VFG.Monitor.Auto"), "");
    for (const auto& m : list_monitors()) {
        obs_property_list_add_string(mon, m.label.c_str(), m.deviceId.c_str());
    }
    obs_property_set_long_description(mon, obs_module_text("VFG.Hint.Reopen"));

    auto* hud = obs_properties_add_list(coreGrp, "vfg_fps_overlay", obs_module_text("VFG.FpsOverlay"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(hud, obs_module_text("VFG.FpsOverlay.Off"), 0);
    obs_property_list_add_int(hud, obs_module_text("VFG.FpsOverlay.TopLeft"), 1);
    obs_property_list_add_int(hud, obs_module_text("VFG.FpsOverlay.TopRight"), 2);
    obs_property_set_long_description(hud, obs_module_text("VFG.Hint.FpsOverlay"));

    auto* ll = obs_properties_add_bool(coreGrp, "vfg_low_latency", obs_module_text("VFG.LowLatency"));
    obs_property_set_long_description(ll, obs_module_text("VFG.Hint.LowLatency"));
    auto* gp = obs_properties_add_bool(coreGrp, "vfg_gpu_priority", obs_module_text("VFG.GpuPriority"));
    obs_property_set_long_description(gp, obs_module_text("VFG.Hint.GpuPriority"));
    obs_properties_add_group(p, "vfg_grp_core", obs_module_text("VFG.Group.Core"), OBS_GROUP_NORMAL, coreGrp);

    auto* abGrp = obs_properties_create();
    obs_properties_add_bool(abGrp, "vfg_hud_lock", obs_module_text("VFG.HudLock"));
    auto* shot = obs_properties_add_bool(abGrp, "vfg_gpu_shot_change", obs_module_text("VFG.GpuShotChange"));
    obs_property_set_long_description(shot, obs_module_text("VFG.Hint.GpuShotChange"));
    obs_properties_add_bool(abGrp, "vfg_pacing_repair", obs_module_text("VFG.PacingRepair"));
    auto* adaptive = obs_properties_add_bool(abGrp, "vfg_adaptive_fg", obs_module_text("VFG.AdaptiveFG"));
    auto* target = obs_properties_add_list(abGrp, "vfg_adaptive_target_fps", obs_module_text("VFG.AdaptiveTargetFPS"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(target, obs_module_text("VFG.AdaptiveTargetFPS.Auto"), 0);
    for (const int fps : {60, 120, 144, 165, 240})
        obs_property_list_add_int(target, std::to_string(fps).c_str(), fps);
    obs_property_set_modified_callback(adaptive, [](obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
        obs_property_set_visible(obs_properties_get(props, "vfg_adaptive_target_fps"),
            obs_data_get_bool(settings, "vfg_adaptive_fg"));
        return true;
    });
    obs_properties_add_group(p, "vfg_grp_ab", obs_module_text("VFG.Group.AB"), OBS_GROUP_NORMAL, abGrp);

    auto* ctrlGrp = obs_properties_create();
    bool running = false;
    if (opaque) {
        auto* tap = static_cast<Tap*>(opaque);
        std::lock_guard lock(tap->command);
        running = tap->controller && tap->controller->alive() && !tap->stopping;
    }
    auto* st = obs_properties_add_text(ctrlGrp, "vfg_status", obs_module_text("VFG.Status.MissingRuntime"), OBS_TEXT_INFO);
    obs_property_set_enabled(st, false);
    if (running) {
        auto* active = obs_properties_add_text(ctrlGrp, "vfg_active_status",
            obs_module_text("VFG.Status.ActivePreview"), OBS_TEXT_INFO);
        obs_property_set_enabled(active, false);
    }

    obs_properties_add_button2(ctrlGrp, "vfg_start", obs_module_text("VFG.Open"),
        [](obs_properties_t*, obs_property_t*, void* data) { return data && start(*static_cast<Tap*>(data), false); }, opaque);
    obs_properties_add_button2(ctrlGrp, "vfg_off", obs_module_text("VFG.OpenOff"),
        [](obs_properties_t*, obs_property_t*, void* data) { return data && start(*static_cast<Tap*>(data), true); }, opaque);
    obs_properties_add_button2(ctrlGrp, "vfg_stop", obs_module_text("VFG.Close"),
        [](obs_properties_t*, obs_property_t*, void* data) { if (!data) return false; stop(data); return true; }, opaque);
    auto* hideCapture = obs_properties_add_bool(ctrlGrp, "vfg_hide_from_capture", obs_module_text("VFG.HideFromCapture"));
    obs_property_set_long_description(hideCapture, obs_module_text("VFG.Hint.HideFromCapture"));
    auto* hideCanvas = obs_properties_add_bool(ctrlGrp, "vfg_hide_source_canvas", obs_module_text("VFG.HideSourceCanvas"));
    obs_property_set_long_description(hideCanvas, obs_module_text("VFG.Hint.HideSourceCanvas"));
    obs_properties_add_group(p, "vfg_grp_control", obs_module_text("VFG.Group.Control"), OBS_GROUP_NORMAL, ctrlGrp);
    split::add_properties(p);
    auto* upscale = obs_properties_create();
    auto* compare = obs_properties_add_bool(upscale, "vfg_upscale_compare", obs_module_text("VFG.Upscale.Compare"));
    obs_property_set_long_description(compare, obs_module_text("VFG.Upscale.CompareHint"));
    obs_upscale::add_properties(upscale);
    obs_properties_add_group(p, "vfg_upscale", obs_module_text("VFG.Group.Upscale"), OBS_GROUP_CHECKABLE, upscale);
    return p;
}
} // namespace
} // namespace nr::fg::vfg

extern "C" void nr_register_vfg_tap()
{
    using namespace nr::fg::vfg;
    namespace obs_upscale = nr::obs_upscale;
    obs_source_info info{};
    info.id = "susemi_fg_tap";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = [](void*) -> const char* { return obs_module_text("VFG.Name"); };
    info.get_defaults = [](obs_data_t* settings) {
        obs_data_set_default_int(settings, "vfg_multiplier", 2);
        obs_data_set_default_int(settings, qualityKey, static_cast<long long>(Quality::Medium));
        obs_data_set_default_int(settings, dedupModeKey, static_cast<long long>(DedupMode::Auto));
        obs_data_set_default_bool(settings, liveAnchorKey, true);
        obs_data_set_default_string(settings, "vfg_monitor", "");
        obs_data_set_default_int(settings, "vfg_fps_overlay", 1);
        obs_data_set_default_bool(settings, "vfg_low_latency", true);
        obs_data_set_default_bool(settings, "vfg_gpu_priority", true);
        obs_data_set_default_bool(settings, "vfg_hide_from_capture", false);
        obs_data_set_default_bool(settings, "vfg_hide_source_canvas", false);
        obs_data_set_default_int(settings, "vfg_backend", static_cast<long long>(Backend::NativeVfg));
        obs_data_set_default_int(settings, "vfg_pushflow_tier", static_cast<long long>(PushFlowTier::Fast));
        obs_data_set_default_bool(settings, "vfg_experimental_anr", false);
        obs_data_set_default_string(settings, "vfg_anr_dll_path", "");
        obs_data_set_default_bool(settings, "vfg_hud_lock", false);
        obs_data_set_default_bool(settings, "vfg_gpu_shot_change", false);
        obs_data_set_default_bool(settings, "vfg_pacing_repair", false);
        obs_data_set_default_bool(settings, "vfg_adaptive_fg", false);
        obs_data_set_default_int(settings, "vfg_adaptive_target_fps", 0);
        obs_upscale::defaults(settings);
        nr::split::defaults(settings);
        obs_data_set_default_bool(settings, "vfg_upscale", false);
        obs_data_set_default_bool(settings, "vfg_upscale_compare", false);
    };
    info.create = [](obs_data_t* settings, obs_source_t* source) -> void* {
#ifdef OBS_NR_VFG_QA_ACTION
        obs_data_unset_user_value(settings, qaKey);
#endif
        auto* tap = new (std::nothrow) Tap(source);
        if (tap && settings) {
            tap->upscale = obs_upscale::create(settings, source);
            apply_settings(*tap, settings);
        }
        return tap;
    };
#ifdef OBS_NR_VFG_QA_ACTION
    init_qa_action();
    info.update = qa_update;
    info.load = [](void* data, obs_data_t* settings) {
        obs_data_unset_user_value(settings, qaKey);
        if (data && settings) apply_settings(*static_cast<Tap*>(data), settings);
    };
#else
    info.update = [](void* data, obs_data_t* settings) {
        if (data && settings) apply_settings(*static_cast<Tap*>(data), settings);
    };
    info.load = [](void* data, obs_data_t* settings) {
        if (data && settings) apply_settings(*static_cast<Tap*>(data), settings);
    };
#endif
    info.filter_remove = [](void* data, obs_source_t*) {
        if (!data) return;
        static_cast<Tap*>(data)->detached.store(true);
        stop(data);
    };
    info.destroy = [](void* data) {
        auto* tap = static_cast<Tap*>(data);
        obs_enter_graphics();
        if (tap->controller) disconnect(*tap, "filter_destroyed", true);
        gs_texrender_destroy(tap->capture);
        tap->splitCanvas.clear();
        obs_leave_graphics();
        obs_upscale::destroy(tap->upscale);
        delete tap;
    };
    info.get_width = [](void* data) {
        auto* tap = static_cast<Tap*>(data);
        return tap->upscale ? obs_upscale::dimensions(*tap->upscale).width :
            obs_source_get_base_width(obs_filter_get_target(tap->source));
    };
    info.get_height = [](void* data) {
        auto* tap = static_cast<Tap*>(data);
        return tap->upscale ? obs_upscale::dimensions(*tap->upscale).height :
            obs_source_get_base_height(obs_filter_get_target(tap->source));
    };
    info.video_get_color_space = [](void* data, size_t count, const gs_color_space* preferred) {
        auto* target = obs_filter_get_target(static_cast<Tap*>(data)->source);
        return target ? obs_source_get_color_space(target, count, preferred) : GS_CS_SRGB;
    };
    info.video_render = render;
    info.get_properties = properties;
    obs_register_source(&info);
}
