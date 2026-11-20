// SPDX-License-Identifier: GPL-3.0-only
#include "nr_contract.h"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <util/bmem.h>
#include <atomic>
#include <new>

namespace {
constexpr char modeKey[] = "output_mode";
constexpr char sharpnessKey[] = "cas_strength";
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

struct Filter {
    obs_source_t* source;
    gs_effect_t* effect = nullptr;
    gs_eparam_t* outputStep = nullptr;
    gs_eparam_t* sharpness = nullptr;
    gs_eparam_t* multiplier = nullptr;
    std::atomic<uint32_t> settings{0}; // low 2 bits: mode; remaining bits: strength in percent
};

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

nr::Extent dimensions(const Filter& f)
{
    auto* target = obs_filter_get_target(f.source);
    const auto in = input_extent(target);
    if (!target || !f.effect || !sdr(obs_source_get_color_space(target, OBS_COUNTOF(spaces), spaces)))
        return in;
    return output_extent(in, f.settings.load(std::memory_order_relaxed) & 3u);
}

void update(void* data, obs_data_t* settings)
{
    auto& f = *static_cast<Filter*>(data);
    const auto rawMode = obs_data_get_int(settings, modeKey);
    const auto mode = rawMode >= 0 && rawMode <= native ? static_cast<uint32_t>(rawMode) : 0u;
    const auto strength = std::clamp(obs_data_get_int(settings, sharpnessKey), 0LL, 100LL);
    f.settings.store(mode | (static_cast<uint32_t>(strength) << 2), std::memory_order_relaxed);
}

void* create(obs_data_t* settings, obs_source_t* source)
{
    auto* f = new (std::nothrow) Filter{source};
    if (!f)
        return nullptr;
    update(f, settings);
    char* error = nullptr;
    obs_enter_graphics();
    f->effect = gs_effect_create(effectCode, "obs-nr-upscale.effect", &error);
    if (f->effect) {
        f->outputStep = gs_effect_get_param_by_name(f->effect, "output_step");
        f->sharpness = gs_effect_get_param_by_name(f->effect, "sharpness");
        f->multiplier = gs_effect_get_param_by_name(f->effect, "multiplier");
    }
    obs_leave_graphics();
    if (!f->effect)
        blog(LOG_WARNING, "obs-nr-upscale: spatial effect unavailable, passing through: %s", error ? error : "");
    bfree(error);
    return f;
}

void destroy(void* data)
{
    auto* f = static_cast<Filter*>(data);
    obs_enter_graphics();
    if (f->effect)
        gs_effect_destroy(f->effect);
    obs_leave_graphics();
    delete f;
}

void render(void* data, gs_effect_t*)
{
    auto& f = *static_cast<Filter*>(data);
    auto* target = obs_filter_get_target(f.source);
    if (!target || !obs_filter_get_parent(f.source))
        return;
    const uint32_t settings = f.settings.load(std::memory_order_relaxed);
    const uint32_t mode = settings & 3u;
    const uint32_t strength = settings >> 2;
    const auto in = input_extent(target);
    const auto out = output_extent(in, mode);
    const auto space = obs_source_get_color_space(target, OBS_COUNTOF(spaces), spaces);
    if (!f.effect || !in.width || !in.height || !sdr(space) || mode == 0 || (in == out && strength == 0)) {
        obs_source_skip_video_filter(f.source);
        return;
    }

    if (!obs_source_process_filter_begin_with_color_space(f.source, gs_get_format_from_space(space), space,
                                                          OBS_NO_DIRECT_RENDERING))
        return;
    const auto step = vec2{1.0f / static_cast<float>(out.width), 1.0f / static_cast<float>(out.height)};
    const float multiplier = gs_get_color_space() == GS_CS_709_SCRGB ? obs_get_video_sdr_white_level() / 80.0f : 1.0f;
    gs_effect_set_vec2(f.outputStep, &step);
    gs_effect_set_float(f.sharpness, static_cast<float>(strength) / 100.0f);
    gs_effect_set_float(f.multiplier, multiplier);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_source_process_filter_end(f.source, f.effect, out.width, out.height);
    gs_blend_state_pop();
}

void defaults(obs_data_t* settings)
{
    obs_data_set_default_int(settings, modeKey, native);
    obs_data_set_default_int(settings, sharpnessKey, 35);
}

obs_properties_t* properties(void*)
{
    auto* props = obs_properties_create();
    auto* modes = obs_properties_add_list(props, modeKey, obs_module_text("Upscale.Output"),
                                         OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(modes, obs_module_text("Upscale.Off"), 0);
    obs_property_list_add_int(modes, obs_module_text("Upscale.Native"), native);
    obs_property_list_add_int(modes, "1440p", 1);
    obs_property_list_add_int(modes, "4K", 2);
    obs_properties_add_int_slider(props, sharpnessKey, obs_module_text("Upscale.Strength"), 0, 100, 1);
    obs_properties_add_text(props, "upscale_info", obs_module_text("Upscale.Hint"), OBS_TEXT_INFO);
    return props;
}
}

extern "C" void nr_register_upscale_filter(void)
{
    obs_source_info info{};
    info.id = "susemi_upscale_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = [](void*) { return obs_module_text("Upscale.Name"); };
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.get_width = [](void* data) { return dimensions(*static_cast<Filter*>(data)).width; };
    info.get_height = [](void* data) { return dimensions(*static_cast<Filter*>(data)).height; };
    info.video_get_color_space = [](void* data, size_t count, const gs_color_space* preferred) {
        auto* target = obs_filter_get_target(static_cast<Filter*>(data)->source);
        return target ? obs_source_get_color_space(target, count, preferred) : GS_CS_SRGB;
    };
    info.video_render = render;
    obs_register_source(&info);
}
