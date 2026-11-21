// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include "fg_monitor_text.h"
#include "../obs_upscale_filter.h"
#include "../obs_quality_trace.h"
#include <obs-module.h>
#include <graphics/vec4.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#ifdef OBS_NR_VFG_QA_ACTION
#include <charconv>
#include <string_view>
#endif

namespace nr::fg::vfg {
namespace {
using Microsoft::WRL::ComPtr;

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
    gs_texrender_t* capture{};
    obs_source_t* target{};
    uint32_t previousWidth{}, previousHeight{};
    uint32_t activeWidth{}, activeHeight{};
    uint32_t multiplier{2};
    uint32_t quality{1};
    uint32_t fpsOverlay{1};
    uint32_t lowLatency{1};
    uint32_t gpuPriority{1};
    std::string monitorId;
    uint64_t lastTick{};
    uint64_t retiredGeneration{};
    bool breakPair{};
    bool resizing{};
    bool stopping{true};
    std::atomic_bool detached{false};
};
std::atomic_uint64_t nextTap{0};

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

void disconnect(Tap& tap, const char* cause) noexcept
{
    if (tap.controller) tap.controller->stop();
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
    tap.stopping = true;
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

// Returns a displayable capture, if this draw also supplied the presenter.
gs_texture_t* publish(Tap& tap, obs_source_t* target, obs_source_t* parent, Extent output)
{
    if (!obs_in_task_thread(OBS_TASK_GRAPHICS)) return nullptr;
    std::unique_lock lock(tap.command, std::try_to_lock);
    if (!lock || !tap.controller) return nullptr;
    if (tap.stopping || tap.detached.load() || !tap.controller->alive()) {
        disconnect(tap, "stopped_or_peer_exit");
        return nullptr;
    }
    const auto tick = obs_get_video_frame_time();
    if (tick == tap.lastTick) return nullptr;
    tap.lastTick = tick;
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
        if (tap.producer && !tap.resizing && (tap.activeWidth != w || tap.activeHeight != h)) {
            if (!tap.controller->request_resize()) {
                disconnect(tap, "resize_request");
                return nullptr;
            }
            tap.resizing = true;
            blog(LOG_INFO, "[vfg] resize requested old=%ux%u new=%ux%u generation=%llu",
                tap.activeWidth, tap.activeHeight, w, h, nr::quality::id(tap.producer->generation()));
        }
        if (tap.resizing) {
            if (!tap.controller->resize_drained()) return nullptr;
            const auto retired = tap.producer->retire();
            if (retired == ipc::Status::PeerBusy) return nullptr;
            if (retired != ipc::Status::Ok) {
                disconnect(tap, "resize_retire");
                return nullptr;
            }
            tap.producer.reset();
            tap.resizing = false;
            tap.breakPair = true;
        }
        const ipc::Shape shape{w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
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
            const auto offer = tap.producer->connect(tap.controller->peer());
            tap.target = target;
            tap.activeWidth = w;
            tap.activeHeight = h;
            if (!tap.controller->offer(offer, shape, desc.AdapterLuid)) {
                disconnect(tap, "offer_rejected");
                return nullptr;
            }
            blog(LOG_INFO, "[vfg] offered source_uuid=%s tap_uuid=%s size=%ux%u generation=%llu qpc_ns=%llu",
                obs_source_get_uuid(parent), obs_source_get_uuid(tap.source), w, h,
                static_cast<unsigned long long>(offer.generation), static_cast<unsigned long long>(clock_ns()));
        }
        const auto status = tap.producer->status();
        if (status == ipc::Status::Pending) return nullptr;
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
            if (tap.upscale) obs_upscale::render(*tap.upscale);
            else obs_source_skip_video_filter(tap.source);
        }
        auto* texture = gs_texrender_get_texture(tap.capture);
        const auto arrival = clock_ns();
        const auto result = tap.producer->publish(texture ? static_cast<ID3D11Texture2D*>(gs_texture_get_obj(texture)) : nullptr,
            captureQpc, arrival, tap.breakPair, tick);
        if (nr::quality::trace_frames())
            blog(LOG_INFO, "[quality] stage=vfg_tap obs_tick_ns=%llu source=%s generation=%llu capture_qpc_ns=%llu arrival_qpc_ns=%llu status=%d",
                nr::quality::id(tick), obs_source_get_uuid(parent), nr::quality::id(tap.producer->generation()),
                nr::quality::id(captureQpc), nr::quality::id(arrival), static_cast<int>(result));
        if (result == ipc::Status::Ok)
            tap.breakPair = false;
        if (result != ipc::Status::Ok && result != ipc::Status::Dropped) disconnect(tap, "publish_fault");
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
    auto* target = obs_filter_get_target(tap.source);
    auto* parent = obs_filter_get_parent(tap.source);
    if (!target || !parent)
        return;
    const auto size = tap.upscale ? obs_upscale::dimensions(*tap.upscale) :
        Extent{obs_source_get_base_width(target), obs_source_get_base_height(target)};
    auto* texture = publish(tap, target, parent, size);
    if (!texture || gs_get_color_space() != GS_CS_SRGB) {
        if (tap.upscale) obs_upscale::render(*tap.upscale);
        else obs_source_skip_video_filter(tap.source);
        return;
    }
    draw_sdr(texture, size, GS_BLEND_INVSRCALPHA);
}

bool start(Tap& tap, bool off)
{
    if (obs_in_task_thread(OBS_TASK_GRAPHICS)) return false;
    std::lock_guard lock(tap.command);
    if (tap.detached.load() || tap.producer || (tap.controller && !tap.controller->exited()) ||
        (tap.retiredGeneration && ipc::Producer::generation_retained(tap.retiredGeneration))) return false;
    try {
        LaunchOptions opts{};
        opts.multiplier = off ? 1u : std::clamp<uint32_t>(tap.multiplier, 2, max_multiplier);
        opts.quality = std::min<uint32_t>(tap.quality, 2);
        opts.monitorId = utf8_to_wide(tap.monitorId);
        opts.fpsOverlay = std::min<uint32_t>(tap.fpsOverlay, 2);
        opts.lowLatency = tap.lowLatency ? 1u : 0u;
        opts.gpuPriority = tap.gpuPriority ? 1u : 0u;
        tap.controller = std::make_unique<Controller>(opts);
        tap.stopping = false;
        blog(LOG_INFO, "[vfg] open request mode=%ux quality=%u qpc_ns=%llu",
            opts.multiplier, opts.quality, static_cast<unsigned long long>(clock_ns()));
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
        if (tap.controller) tap.controller->stop();
        if (!tap.stopping && tap.producer) queued = obs_source_get_weak_source(tap.source);
        tap.stopping = true;
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
            if (state->stopping && state->producer) disconnect(*state, "close_or_filter_removed");
        }
        obs_leave_graphics();
    }, queued, false);
}

void apply_settings(Tap& tap, obs_data_t* settings)
{
    const auto mult = static_cast<uint32_t>(obs_data_get_int(settings, "vfg_multiplier"));
    const auto qual = static_cast<uint32_t>(obs_data_get_int(settings, "vfg_quality"));
    const auto hud = static_cast<uint32_t>(std::clamp<long long>(obs_data_get_int(settings, "vfg_fps_overlay"), 0, 2));
    const uint32_t ll = obs_data_get_bool(settings, "vfg_low_latency") ? 1u : 0u;
    const uint32_t gp = obs_data_get_bool(settings, "vfg_gpu_priority") ? 1u : 0u;
    const char* mon = obs_data_get_string(settings, "vfg_monitor");
    const bool enabled = obs_data_get_bool(settings, "vfg_upscale");
    const bool compare = obs_data_get_bool(settings, "vfg_upscale_compare");
    std::lock_guard lock(tap.command);
    if (obs_upscale::update(tap.upscale, settings, enabled && !compare, enabled))
        tap.breakPair = true;
    tap.multiplier = std::clamp<uint32_t>(mult ? mult : 2, 2, max_multiplier);
    tap.quality = std::min<uint32_t>(qual, 2);
    tap.fpsOverlay = hud;
    tap.lowLatency = ll;
    tap.gpuPriority = gp;
    tap.monitorId = mon ? mon : "";
    if (tap.controller) {
        tap.controller->set_fps_overlay(tap.fpsOverlay);
        tap.controller->set_low_latency(tap.lowLatency);
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

obs_properties_t* properties(void* opaque)
{
    auto* p = obs_properties_create();
    auto* coreGrp = obs_properties_create();
    auto* mult = obs_properties_add_list(coreGrp, "vfg_multiplier", obs_module_text("VFG.Multiplier"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mult, "2x (120fps)", 2);
    obs_property_list_add_int(mult, "3x (180fps)", 3);
    obs_property_list_add_int(mult, "4x (240fps)", 4);
    obs_property_set_long_description(mult, obs_module_text("VFG.Hint.Multiplier"));

    auto* qual = obs_properties_add_list(coreGrp, "vfg_quality", obs_module_text("VFG.Quality"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(qual, obs_module_text("VFG.Quality.Best"), 0);
    obs_property_list_add_int(qual, obs_module_text("VFG.Quality.Balanced"), 1);
    obs_property_list_add_int(qual, obs_module_text("VFG.Quality.Fast"), 2);
    obs_property_set_long_description(qual, obs_module_text("VFG.Hint.Reopen"));

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

    auto* ctrlGrp = obs_properties_create();
    const bool rtFound = !Controller::find_runtime_dir().empty();
    bool running = false;
    if (opaque) {
        auto* tap = static_cast<Tap*>(opaque);
        std::lock_guard lock(tap->command);
        running = tap->controller && tap->controller->alive() && !tap->stopping;
    }
    const char* statusKey = running ? "VFG.Status.Running" : (rtFound ? "VFG.Status.Ready" : "VFG.Status.MissingRuntime");
    auto* st = obs_properties_add_text(ctrlGrp, "vfg_status", obs_module_text(statusKey), OBS_TEXT_INFO);
    obs_property_set_enabled(st, false);

    obs_properties_add_button2(ctrlGrp, "vfg_start", obs_module_text("VFG.Open"),
        [](obs_properties_t*, obs_property_t*, void* data) { return data && start(*static_cast<Tap*>(data), false); }, opaque);
    obs_properties_add_button2(ctrlGrp, "vfg_off", obs_module_text("VFG.OpenOff"),
        [](obs_properties_t*, obs_property_t*, void* data) { return data && start(*static_cast<Tap*>(data), true); }, opaque);
    obs_properties_add_button2(ctrlGrp, "vfg_stop", obs_module_text("VFG.Close"),
        [](obs_properties_t*, obs_property_t*, void* data) { if (!data) return false; stop(data); return true; }, opaque);
    obs_properties_add_group(p, "vfg_grp_control", obs_module_text("VFG.Group.Control"), OBS_GROUP_NORMAL, ctrlGrp);
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
        obs_data_set_default_int(settings, "vfg_quality", 1);
        obs_data_set_default_string(settings, "vfg_monitor", "");
        obs_data_set_default_int(settings, "vfg_fps_overlay", 1);
        obs_data_set_default_bool(settings, "vfg_low_latency", true);
        obs_data_set_default_bool(settings, "vfg_gpu_priority", true);
        obs_upscale::defaults(settings);
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
        if (tap->controller) disconnect(*tap, "filter_destroyed");
        gs_texrender_destroy(tap->capture);
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
