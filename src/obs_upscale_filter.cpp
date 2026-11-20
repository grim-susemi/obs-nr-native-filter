// SPDX-License-Identifier: GPL-3.0-only
#include "obs_upscale_filter.h"
#include "obs_upscale_dlss.h"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <atomic>
#include <memory>
#include <new>

namespace nr::obs_upscale {
namespace {
constexpr char modeKey[] = "output_mode";
constexpr char sharpnessKey[] = "cas_strength";
constexpr char backendKey[] = "scaling_backend";
constexpr char modelKey[] = "dlss_model";
constexpr char runtimeKey[] = "dlss_runtime_path";
constexpr char dlssHintKey[] = "dlss_hint";
constexpr uint32_t native = 3;

// Same five-tap CAS weights as nr_cas.hlsl, adapted to OBS's premultiplied
// linear sampling. Output-pixel offsets fuse bilinear resize and CAS.
// AMD FidelityFX CAS algorithm copyright (c) 2020 Advanced Micro Devices, Inc.
// See provenance/source-notices/AMD-FidelityFX-CAS-MIT.txt.
constexpr char effectCode[] = R"effect(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 output_step;
uniform float sharpness;
uniform float multiplier;

sampler_state linear_clamp {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertData {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};
VertData VSMain(VertData v) {
    v.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
    return v;
}
float3 straight(float4 c) {
    return c.rgb / max(c.a, 0.0001);
}
float3 neighbor(float2 uv, float3 center) {
    float4 sampleColor = image.Sample(linear_clamp, uv);
    // Invisible neighbors must not contribute dark or colored halos.
    return lerp(center, saturate(straight(sampleColor)), sampleColor.a);
}
float4 PSCas(VertData v) : TARGET {
    float4 raw = image.Sample(linear_clamp, v.uv);
    if (sharpness <= 0.0 || raw.a <= 0.0001)
        return float4(raw.rgb * multiplier, raw.a);
    float3 c = saturate(straight(raw));
    float3 a = neighbor(v.uv - float2(0.0, output_step.y), c);
    float3 b = neighbor(v.uv - float2(output_step.x, 0.0), c);
    float3 d = neighbor(v.uv + float2(output_step.x, 0.0), c);
    float3 e = neighbor(v.uv + float2(0.0, output_step.y), c);
    float3 lo = min(c, min(min(a, e), min(b, d)));
    float3 hi = max(c, max(max(a, e), max(b, d)));
    float3 amp = sqrt(saturate(min(lo, 2.0 - hi) / max(hi, 0.0001)));
    float3 w = -amp / lerp(8.0, 5.0, sharpness);
    float3 result = saturate((c + (a + b + d + e) * w) / (1.0 + 4.0 * w));
    return float4(result * (raw.a * multiplier), raw.a);
}
technique Draw {
    pass {
        vertex_shader = VSMain(v);
        pixel_shader = PSCas(v);
    }
}
)effect";

struct Settings {
    bool enabled;
    bool keepSize;
    uint32_t mode;
    uint32_t strength;
    bool dlss;
    uint32_t model;
    std::string runtime;
    bool operator==(const Settings&) const = default;
};
}

struct Filter {
    explicit Filter(obs_source_t* source_) : source(source_) {}
    obs_source_t* source;
    gs_effect_t* effect = nullptr;
    gs_eparam_t* outputStep = nullptr;
    gs_eparam_t* sharpness = nullptr;
    gs_eparam_t* multiplier = nullptr;
    gs_eparam_t* image = nullptr;
    ObsUpscaleDlss* dlss = nullptr;
    gs_texrender_t* capture[2]{};
    gs_color_format captureFormat = GS_UNKNOWN;
    uint32_t captureWidth = 0, captureHeight = 0;
    uint64_t ticks[2]{};
    unsigned current = 0;
    obs_source_t* lastTarget = nullptr; // Compare identity only; never dereference after the draw.
    std::shared_ptr<const Settings> lastSettings;
    bool dlssAttempted = false;
    bool publishedDlss = false;
    std::atomic<bool> reset{false};
    std::atomic<std::shared_ptr<const Settings>> settings;
};

namespace {
constexpr gs_color_space spaces[] = {
    GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED, GS_CS_709_SCRGB,
};

bool sdr(gs_color_space space)
{
    return space == GS_CS_SRGB || space == GS_CS_SRGB_16F;
}

nr::Extent input_extent(obs_source_t* target)
{
    return target ? nr::Extent{obs_source_get_base_width(target), obs_source_get_base_height(target)}
                  : nr::Extent{0, 0};
}

nr::Extent output_extent(nr::Extent in, uint32_t mode)
{
    const auto out = nr::upscale_extent(in, mode);
    // D3D11 maximum 2D dimension; extreme aspect ratios can exceed it in cover mode.
    return (out.width > 16384 || out.height > 16384) ? in : out;
}

}

Extent dimensions(const Filter& f)
{
    auto* target = obs_filter_get_target(f.source);
    const auto in = input_extent(target);
    const auto settings = f.settings.load();
    if (!target || !f.effect || !(settings->enabled || settings->keepSize) ||
        !sdr(obs_source_get_color_space(target, OBS_COUNTOF(spaces), spaces)))
        return in;
    return output_extent(in, settings->mode);
}

bool update(Filter* f, obs_data_t* settings, bool enabled, bool keepSize)
{
    if (!f)
        return false;
    const auto rawMode = obs_data_get_int(settings, modeKey);
    const auto mode = rawMode >= 0 && rawMode <= native ? static_cast<uint32_t>(rawMode) : 0u;
    const auto strength = static_cast<uint32_t>(std::clamp(obs_data_get_int(settings, sharpnessKey), 0LL, 100LL));
    const bool dlss = obs_data_get_int(settings, backendKey) == 1;
    const auto rawModel = obs_data_get_int(settings, modelKey);
    const auto model = rawModel >= 0 && rawModel <= 2 ? static_cast<uint32_t>(rawModel) : 1u;
    Settings next{enabled, keepSize, mode, strength, dlss, model, obs_data_get_string(settings, runtimeKey)};
    const auto current = f->settings.load();
    if (current && *current == next)
        return false;
    f->settings.store(std::make_shared<const Settings>(std::move(next)));
    return true;
}

void enabled_changed(void* data, calldata_t*)
{
    static_cast<Filter*>(data)->reset.store(true);
}

Filter* create(obs_data_t* settings, obs_source_t* source)
{
    auto* f = new (std::nothrow) Filter(source);
    if (!f)
        return nullptr;
    update(f, settings, true);
    char* error = nullptr;
    obs_enter_graphics();
    f->effect = gs_effect_create(effectCode, "obs-nr-upscale.effect", &error);
    if (f->effect) {
        f->outputStep = gs_effect_get_param_by_name(f->effect, "output_step");
        f->sharpness = gs_effect_get_param_by_name(f->effect, "sharpness");
        f->multiplier = gs_effect_get_param_by_name(f->effect, "multiplier");
        f->image = gs_effect_get_param_by_name(f->effect, "image");
    }
    obs_leave_graphics();
    if (!f->effect)
        blog(LOG_WARNING, "obs-nr-upscale: spatial effect unavailable, passing through: %s", error ? error : "");
    bfree(error);
    signal_handler_connect(obs_source_get_signal_handler(source), "enable", enabled_changed, f);
    return f;
}

void destroy(Filter* f)
{
    if (!f)
        return;
    signal_handler_disconnect(obs_source_get_signal_handler(f->source), "enable", enabled_changed, f);
    obs_upscale_dlss_destroy(f->dlss);
    obs_enter_graphics();
    for (auto* capture : f->capture)
        gs_texrender_destroy(capture);
    if (f->effect)
        gs_effect_destroy(f->effect);
    obs_leave_graphics();
    delete f;
}

// The libobs filter helper owns its render target privately. Capture explicitly
// for DLSS so the fallback can reuse this exact frame without a second source draw.
static bool draw_dlss(Filter& f, const Settings& settings, nr::Extent in, nr::Extent out, gs_color_space space)
{
    if (!f.dlssAttempted) {
        f.dlss = obs_upscale_dlss_create();
        f.dlssAttempted = true;
    }
    if (!f.dlss)
        return false;
    const auto format = gs_get_format_from_space(space);
    if (f.captureFormat != format || f.captureWidth != in.width || f.captureHeight != in.height) {
        for (auto*& capture : f.capture) {
            gs_texrender_destroy(capture);
            capture = nullptr;
        }
        f.captureFormat = format;
        f.captureWidth = in.width;
        f.captureHeight = in.height;
        f.ticks[0] = f.ticks[1] = 0;
    }
    const auto now = obs_get_video_frame_time();
    const auto interval = obs_get_frame_interval_ns();
    if (!now || !interval)
        return false;
    if (f.ticks[f.current] != now) {
        const auto next = f.current ^ 1u;
        if (!f.capture[next])
            f.capture[next] = gs_texrender_create(format, GS_ZS_NONE);
        f.ticks[next] = 0;
        auto* capture = f.capture[next];
        if (!capture)
            return false;
        gs_texrender_reset(capture);
        if (!gs_texrender_begin_with_color_space(capture, in.width, in.height, space)) {
            obs_upscale_dlss_reset(f.dlss);
            f.ticks[0] = f.ticks[1] = 0;
            return false;
        }
        gs_blend_state_push();
        gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        vec4 clear{};
        gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
        gs_ortho(0, static_cast<float>(in.width), 0, static_cast<float>(in.height), -100, 100);
        obs_source_skip_video_filter(f.source);
        gs_blend_state_pop();
        gs_texrender_end(capture);
        f.current = next;
        f.ticks[next] = now;
    }
    const auto previous = f.current ^ 1u;
    const auto prior = f.ticks[previous];
    // Spatial and temporal output must represent the same OBS tick. A late
    // DLSS completion is never shown after its spatial twin was presented.
    const bool adjacent = prior && now > prior && now - prior <= interval + interval / 2;
    auto* input = gs_texrender_get_texture(f.capture[f.current]);
    auto* spatial = adjacent ? gs_texrender_get_texture(f.capture[previous]) : input;
#if defined(_WIN32) && defined(OBS_NR_VFG_QA_ACTION)
    if (obs_upscale_dlss_qa_legacy_pacing())
        spatial = input;
#endif
    auto* output = obs_upscale_dlss_render(f.dlss, input, adjacent ? prior : 0,
                                          out.width, out.height, settings.model, settings.runtime.c_str());
    const bool linear = gs_set_linear_srgb(true);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    if (output)
        gs_effect_set_texture(f.image, output);
    else
        gs_effect_set_texture_srgb(f.image, spatial);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    while (gs_effect_loop(f.effect, "Draw"))
        gs_draw_sprite(output ? output : spatial, 0, out.width, out.height);
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
    gs_set_linear_srgb(linear);
    if (output && !f.publishedDlss) {
        f.publishedDlss = true;
        blog(LOG_INFO, "obs-nr-upscale: presented DLSS frame (%ux%u -> %ux%u)",
             in.width, in.height, out.width, out.height);
    }
    return true;
}

void render(Filter& f)
{
    auto* target = obs_filter_get_target(f.source);
    if (!target || !obs_filter_get_parent(f.source))
        return;
    const auto settings = f.settings.load();
    const uint32_t mode = settings->mode;
    const uint32_t strength = settings->enabled ? settings->strength : 0u;
    const auto in = input_extent(target);
    const auto out = output_extent(in, mode);
    const auto space = obs_source_get_color_space(target, OBS_COUNTOF(spaces), spaces);
    const bool usable = (settings->enabled || settings->keepSize) && f.effect && in.width && in.height && sdr(space) && mode != 0;
    const bool useDlss = usable && settings->enabled && settings->dlss && (mode == native || in != out);
    if (f.reset.exchange(false) || f.lastTarget != target || !useDlss || !f.lastSettings ||
        f.lastSettings->mode != mode || f.lastSettings->dlss != settings->dlss ||
        f.lastSettings->model != settings->model || f.lastSettings->runtime != settings->runtime) {
        obs_upscale_dlss_reset(f.dlss);
        f.ticks[0] = f.ticks[1] = 0;
        f.publishedDlss = false;
    }
    f.lastTarget = target;
    f.lastSettings = settings;
    if (!usable || (!useDlss && in == out && strength == 0)) {
        obs_source_skip_video_filter(f.source);
        return;
    }

    const auto step = vec2{1.0f / static_cast<float>(out.width), 1.0f / static_cast<float>(out.height)};
    const float multiplier = gs_get_color_space() == GS_CS_709_SCRGB ? obs_get_video_sdr_white_level() / 80.0f : 1.0f;
    gs_effect_set_vec2(f.outputStep, &step);
    gs_effect_set_float(f.sharpness, static_cast<float>(strength) / 100.0f);
    gs_effect_set_float(f.multiplier, multiplier);
    if (useDlss && draw_dlss(f, *settings, in, out, space))
        return;
    if (!obs_source_process_filter_begin_with_color_space(f.source, gs_get_format_from_space(space), space,
                                                          OBS_NO_DIRECT_RENDERING))
        return;
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_source_process_filter_end(f.source, f.effect, out.width, out.height);
    gs_blend_state_pop();
}

void defaults(obs_data_t* settings)
{
    obs_data_set_default_int(settings, modeKey, native);
    obs_data_set_default_int(settings, sharpnessKey, 35);
    obs_data_set_default_int(settings, backendKey, 0);
    obs_data_set_default_int(settings, modelKey, 1);
    obs_data_set_default_string(settings, runtimeKey, "");
}

static bool sync_controls(obs_properties_t* props, obs_property_t*, obs_data_t* settings)
{
    const bool enabled = obs_data_get_int(settings, modeKey) != 0;
    const bool dlss = enabled && obs_data_get_int(settings, backendKey) == 1;
    obs_property_set_enabled(obs_properties_get(props, backendKey), enabled);
    obs_property_set_enabled(obs_properties_get(props, sharpnessKey), enabled);
    for (const auto* key : {modelKey, dlssHintKey})
        obs_property_set_visible(obs_properties_get(props, key), dlss);
    return true;
}

void add_properties(obs_properties_t* props)
{
    auto* modes = obs_properties_add_list(props, modeKey, obs_module_text("Upscale.Output"),
                                         OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(modes, obs_module_text("Upscale.Off"), 0);
    obs_property_list_add_int(modes, obs_module_text("Upscale.Native"), native);
    obs_property_list_add_int(modes, "1440p", 1);
    obs_property_list_add_int(modes, "4K", 2);
    obs_property_set_modified_callback(modes, sync_controls);
    auto* backend = obs_properties_add_list(props, backendKey, obs_module_text("Upscale.Backend"),
                                           OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(backend, obs_module_text("Upscale.Bilinear"), 0);
    obs_property_list_add_int(backend, obs_module_text("Upscale.Dlss"), 1);
    obs_property_set_modified_callback(backend, sync_controls);
    auto* model = obs_properties_add_list(props, modelKey, obs_module_text("Upscale.DlssModel"),
                                         OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(model, obs_module_text("Upscale.DlssModelK"), 0);
    obs_property_list_add_int(model, obs_module_text("Upscale.DlssModelM"), 1);
    obs_property_list_add_int(model, obs_module_text("Upscale.DlssModelL"), 2);
    obs_properties_add_text(props, dlssHintKey, obs_module_text("Upscale.DlssHint"), OBS_TEXT_INFO);
    obs_properties_add_int_slider(props, sharpnessKey, obs_module_text("Upscale.Strength"), 0, 100, 1);
    obs_properties_add_text(props, "upscale_info", obs_module_text("Upscale.Hint"), OBS_TEXT_INFO);
}
}

extern "C" void nr_register_upscale_filter(void)
{
    using namespace nr::obs_upscale;
    obs_source_info info{};
    info.id = "susemi_upscale_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_DEPRECATED;
    info.get_name = [](void*) { return obs_module_text("Upscale.Name"); };
    info.create = [](obs_data_t* settings, obs_source_t* source) -> void* { return create(settings, source); };
    info.destroy = [](void* data) { destroy(static_cast<Filter*>(data)); };
    info.update = [](void* data, obs_data_t* settings) { update(static_cast<Filter*>(data), settings, true); };
    info.get_defaults = defaults;
    info.get_properties = [](void*) {
        auto* props = obs_properties_create();
        add_properties(props);
        return props;
    };
    info.get_width = [](void* data) { return dimensions(*static_cast<Filter*>(data)).width; };
    info.get_height = [](void* data) { return dimensions(*static_cast<Filter*>(data)).height; };
    info.video_get_color_space = [](void* data, size_t count, const gs_color_space* preferred) {
        auto* target = obs_filter_get_target(static_cast<Filter*>(data)->source);
        return target ? obs_source_get_color_space(target, count, preferred) : GS_CS_SRGB;
    };
    info.video_render = [](void* data, gs_effect_t*) { render(*static_cast<Filter*>(data)); };
    obs_register_source(&info);
}
