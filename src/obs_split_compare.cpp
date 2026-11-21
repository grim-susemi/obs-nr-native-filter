// SPDX-License-Identifier: GPL-3.0-only
#include "obs_split_compare.h"
#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace nr::split {
namespace {
constexpr char enabledKey[] = "split_compare_enabled";
constexpr char positionKey[] = "split_compare_position";
std::atomic<unsigned> requests{};
thread_local obs_source_t* suppressedParent{};
struct OriginalRender {
    obs_source_t* parent;
    bool reentered{};
};
thread_local OriginalRender* originalRender{};

constexpr char splitEffect[] = R"effect(
uniform float4x4 ViewProj;
uniform texture2d before_image;
uniform texture2d after_image;
uniform float split_x;
uniform float viewport_width;
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
float4 PSMain(VertData v) : TARGET {
    float x = v.uv.x * viewport_width;
    float divider_start = clamp(floor(split_x) - 1.0, 0.0, max(viewport_width - 2.0, 0.0));
    if (x >= divider_start && x < divider_start + 2.0)
        return float4(0.0, 1.0, 0.0, 1.0);
    return x < split_x ? before_image.Sample(linear_clamp, v.uv)
                       : after_image.Sample(linear_clamp, v.uv);
}
technique Draw {
    pass {
        vertex_shader = VSMain(v);
        pixel_shader = PSMain(v);
    }
}
)effect";

void draw_texture(gs_texture_t* texture, Extent extent)
{
    auto* effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, extent.width, extent.height);
}

void draw_original(void* opaque)
{
    auto& raw = *static_cast<OriginalRender*>(opaque);
    const auto flags = obs_source_get_output_flags(raw.parent);
    if (!(flags & (OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_ASYNC))) {
        obs_source_default_render(raw.parent);
        return;
    }
    // libobs already bypasses the parent's effect list while inside that list.
    // Re-entering the parent directly avoids reading another filter's mutable
    // attachment/target pointers after enumeration has released the list lock.
    auto* previous = originalRender;
    originalRender = &raw;
    obs_source_video_render(raw.parent);
    originalRender = previous;
}
}

void defaults(obs_data_t* data)
{
    obs_data_set_default_bool(data, enabledKey, false);
    obs_data_set_default_int(data, positionKey, 50);
}

void add_properties(obs_properties_t* props)
{
    auto* enabled = obs_properties_add_bool(props, enabledKey, obs_module_text("SplitCompare.Enabled"));
    obs_property_set_long_description(enabled, obs_module_text("SplitCompare.Hint"));
    obs_properties_add_int_slider(props, positionKey, obs_module_text("SplitCompare.Position"), 0, 100, 1);
}

void Setting::update(obs_data_t* data)
{
    const bool enabled = obs_data_get_bool(data, enabledKey);
    if (enabled == enabled_)
        return;
    enabled_ = enabled;
    if (enabled)
        requests.fetch_add(1, std::memory_order_relaxed);
    else
        requests.fetch_sub(1, std::memory_order_relaxed);
}

Setting::~Setting()
{
    if (enabled_)
        requests.fetch_sub(1, std::memory_order_relaxed);
}

Plan plan(obs_source_t* filter)
{
    if (!requests.load(std::memory_order_relaxed))
        return {};
    auto* parent = obs_filter_get_parent(filter);
    if (!parent || parent == suppressedParent)
        return {};
    struct Scan {
        obs_source_t* last{};
        int percent = -1;
    } scan;
    // libobs enumerates from input to output.
    obs_source_enum_filters(parent, [](obs_source_t*, obs_source_t* candidate, void* data) {
        auto& s = *static_cast<Scan*>(data);
        if (!obs_source_enabled(candidate))
            return;
        const auto* id = obs_source_get_unversioned_id(candidate);
        if (!id || (std::strcmp(id, "susemi_nr_filter") && std::strcmp(id, "susemi_upscale_filter") &&
                    std::strcmp(id, "susemi_fg_tap")))
            return;
        s.last = candidate;
        auto* settings = obs_source_get_settings(candidate);
        if (obs_data_get_bool(settings, enabledKey))
            s.percent = static_cast<int>(std::clamp(obs_data_get_int(settings, positionKey), 0LL, 100LL));
        obs_data_release(settings);
    }, &scan);
    if (scan.last != filter || scan.percent < 0)
        return {};
    return {parent, static_cast<uint32_t>(scan.percent)};
}

bool capturing_original(obs_source_t* filter)
{
    if (!originalRender || obs_filter_get_parent(filter) != originalRender->parent)
        return false;
    originalRender->reentered = true;
    return true;
}

bool capturing_processed(obs_source_t* filter)
{
    return suppressedParent && suppressedParent == obs_filter_get_parent(filter);
}

Suppress::Suppress(const Plan& plan) : previous_(suppressedParent)
{
    suppressedParent = plan.parent;
}

Suppress::~Suppress() { suppressedParent = previous_; }

void Buffer::clear()
{
    gs_texrender_destroy(target_);
    target_ = nullptr;
}

gs_texture_t* Buffer::capture(Extent size, gs_color_space space, Draw render, void* context)
{
    if (!size.width || !size.height)
        return nullptr;
    const auto format = gs_get_format_from_space(space);
    if (target_ && gs_texrender_get_format(target_) != format)
        clear();
    if (!target_)
        target_ = gs_texrender_create(format, GS_ZS_NONE);
    if (!target_)
        return nullptr;
    gs_texrender_reset(target_);
    if (!gs_texrender_begin_with_color_space(target_, size.width, size.height, space))
        return nullptr;
    const bool srgb = gs_framebuffer_srgb_enabled();
    const bool linear = gs_get_linear_srgb();
    const auto cull = gs_get_cull_mode();
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    const vec4 clearColor{};
    gs_clear(GS_CLEAR_COLOR, &clearColor, 0, 0);
    gs_ortho(0, float(size.width), 0, float(size.height), -100, 100);
    render(context);
    gs_blend_state_pop();
    gs_set_cull_mode(cull);
    gs_set_linear_srgb(linear);
    gs_enable_framebuffer_srgb(srgb);
    gs_texrender_end(target_);
    return gs_texrender_get_texture(target_);
}

void Canvas::clear()
{
    before_.clear();
    after_.clear();
    scaled_.clear();
    if (effect_)
        gs_effect_destroy(effect_);
    effect_ = nullptr;
    effectAttempted_ = false;
}

gs_texture_t* Canvas::original(const Plan& plan, Extent size, gs_color_space space, bool scale)
{
    auto* parent = obs_source_get_ref(plan.parent);
    if (!parent)
        return nullptr;
    const auto rawSize = Extent{obs_source_get_base_width(parent), obs_source_get_base_height(parent)};
    OriginalRender render{parent};
    auto* raw = before_.capture(rawSize, space, draw_original, &render);
    obs_source_release(parent);
    if (render.reentered)
        return nullptr;
    if (!scale || !raw || size == rawSize)
        return raw;
    struct Scale { gs_texture_t* texture; Extent size; } context{raw, size};
    return scaled_.capture(size, space, [](void* ctx) {
        const auto& value = *static_cast<Scale*>(ctx);
        gs_enable_blending(false);
        gs_enable_framebuffer_srgb(false);
        draw_texture(value.texture, value.size);
    }, &context);
}

bool Canvas::draw(gs_texture_t* before, gs_texture_t* after, Extent size, uint32_t percent)
{
    if (!before || !after)
        return false;
    if (!effectAttempted_) {
        effectAttempted_ = true;
        char* errors = nullptr;
        effect_ = gs_effect_create(splitEffect, "obs-nr-split.effect", &errors);
        if (!effect_)
            blog(LOG_WARNING, "obs-nr: split effect unavailable: %s", errors ? errors : "");
        bfree(errors);
    }
    if (!effect_)
        return false;
    const bool encode = gs_get_color_space() == GS_CS_SRGB;
    const auto bind = encode ? gs_effect_set_texture_srgb : gs_effect_set_texture;
    bind(gs_effect_get_param_by_name(effect_, "before_image"), before);
    bind(gs_effect_get_param_by_name(effect_, "after_image"), after);
    gs_effect_set_float(gs_effect_get_param_by_name(effect_, "viewport_width"), float(size.width));
    gs_effect_set_float(gs_effect_get_param_by_name(effect_, "split_x"), float(size.width) * float(percent) / 100.0f);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(encode);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    while (gs_effect_loop(effect_, "Draw"))
        gs_draw_sprite(after, 0, size.width, size.height);
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
    return true;
}

bool Canvas::render(const Plan& plan, Extent size, Draw render, void* context)
{
    const auto space = gs_get_color_space();
    auto* before = original(plan, size, space, false);
    if (!before)
        return false;
    gs_texture_t* after;
    {
        const Suppress suppress(plan);
        after = after_.capture(size, space, render, context);
    }
    if (!after)
        return false;
    if (!draw(before, after, size, plan.percent)) {
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        draw_texture(after, size);
        gs_blend_state_pop();
    }
    return true;
}
}
