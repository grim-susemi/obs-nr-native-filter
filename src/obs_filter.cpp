// SPDX-License-Identifier: GPL-3.0-only
#include "obs_filter.h"
#include <obs-module.h>
#include <graphics/vec4.h>
#include <atomic>

namespace {
using nr::sampler::Admission;
std::atomic<uint64_t> nextInstance{1};
struct Filter {
    obs_source_t *source;
    const uint64_t instance;
    nr::PerInstanceTick ticks;
    gs_texrender_t *capture = nullptr;
    uint64_t ingress = 0, clock = 0, interval = 0;
    uint32_t width = 0, height = 0;
    std::atomic<Admission> admission{Admission::NoTarget};
    explicit Filter(obs_source_t *s) : source(s), instance(nextInstance.fetch_add(1)), ticks(instance) {}
};
const char *status(Admission value)
{
    switch (value) {
    case Admission::Ready: return "Sampler only: pass-through; NR inference unavailable";
    case Admission::NoTarget: return "Bypassed: no target";
    case Admission::ZeroExtent: return "Bypassed: zero extent";
    case Admission::UnsupportedRenderer: return "Bypassed: unsupported renderer (D3D11 required)";
    case Admission::UnsupportedColor: return "Bypassed: unsupported sampler color space";
    case Admission::CaptureFailed: return "Bypassed: capture failed";
    default: return "Bypassed: OBS video clock unavailable";
    }
}
// Read-only observation of production state, on the graphics owner thread.
void snapshot(void *opaque, calldata_t *data)
{
    const auto &f = *static_cast<Filter *>(opaque);
    calldata_set_int(data, "instance", f.instance);
    calldata_set_int(data, "tick", nr::sampler::canonical_tick(f.clock, f.interval));
    calldata_set_int(data, "clock", f.clock);
    calldata_set_int(data, "interval", f.interval);
    calldata_set_int(data, "ingress", f.ingress);
    calldata_set_int(data, "width", f.width);
    calldata_set_int(data, "height", f.height);
    calldata_set_int(data, "admission", static_cast<int>(f.admission.load()));
    calldata_set_string(data, "status", status(f.admission));
    const auto *selection = f.ticks.peek(nr::sampler::canonical_tick(f.clock, f.interval));
    calldata_set_int(data, "selected", selection && selection->frame ? selection->frame->ingressSequence : 0);
    calldata_set_int(data, "epoch", selection && selection->frame ? selection->frame->contentEpoch : 0);
    calldata_set_string(data, "color", f.admission == Admission::Ready ? "Rec709Srgb" : "Unspecified");
    calldata_set_string(data, "alpha", f.admission == Admission::Ready ? "Premultiplied" : "Unspecified");
}
void *create(obs_data_t *, obs_source_t *source)
{
    auto *f = new Filter(source);
    proc_handler_add(obs_source_get_proc_handler(source),
        "void nr_sampler_snapshot(out int instance, out int tick, out int clock, out int interval, out int ingress, out int selected, out int epoch, out int width, out int height, out int admission, out string status, out string color, out string alpha)", snapshot, f);
    return f;
}
void destroy(void *opaque)
{
    auto *f = static_cast<Filter *>(opaque);
    obs_enter_graphics();
    gs_texrender_destroy(f->capture);
    obs_leave_graphics();
    delete f;
}
obs_properties_t *properties(void *opaque)
{
    auto *props = obs_properties_create();
    // The standard OBS filter checkbox controls enablement. Full controls are Task15.
    obs_properties_add_text(props, "sampler_status", status(opaque ? static_cast<Filter *>(opaque)->admission.load() : Admission::NoTarget), OBS_TEXT_INFO);
    return props;
}
void bypass(Filter &f)
{
    // OBS skip dereferences the parent; unattached filters draw nothing.
    if (obs_filter_get_target(f.source) && obs_filter_get_parent(f.source)) obs_source_skip_video_filter(f.source);
}
void render(void *opaque, gs_effect_t *)
{
    auto &f = *static_cast<Filter *>(opaque);
    const auto time = obs_get_video_frame_time();
    const auto interval = obs_get_frame_interval_ns();
    const auto tick = nr::sampler::canonical_tick(time, interval);
    const auto action = f.ticks.claim(tick);
    if (action == nr::TickAction::CaptureOnce) {
        f.clock = time;
        f.interval = interval;
        auto *target = obs_filter_get_target(f.source);
        f.width = target ? obs_source_get_width(target) : 0;
        f.height = target ? obs_source_get_height(target) : 0;
        const gs_color_space preferred[]{GS_CS_SRGB};
        const auto space = target ? obs_source_get_color_space(target, 1, preferred) : GS_CS_SRGB;
        f.admission = nr::sampler::admit(target && obs_filter_get_parent(f.source),
            gs_get_device_type() == GS_DEVICE_DIRECT3D_11, f.width, f.height, space == GS_CS_SRGB);
        if (f.admission == Admission::Ready) {
            if (!f.capture) f.capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
            gs_texrender_reset(f.capture);
            gs_blend_state_push();
            const bool srgb = gs_framebuffer_srgb_enabled();
            gs_enable_framebuffer_srgb(false);
            if (gs_texrender_begin_with_color_space(f.capture, f.width, f.height, space)) {
                vec4 clear{};
                gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
                gs_ortho(0, static_cast<float>(f.width), 0, static_cast<float>(f.height), -100, 100);
                gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
                obs_source_skip_video_filter(f.source);
                gs_texrender_end(f.capture);
                ++f.ingress;
            } else f.admission = Admission::CaptureFailed;
            gs_enable_framebuffer_srgb(srgb);
            gs_blend_state_pop();
        }
        const nr::Selection selected{tick,
            f.admission == Admission::Ready ? nr::OutputKind::PrimingRaw : nr::OutputKind::CurrentSource,
            nr::Status::Bypassed, nr::Reason::None,
            f.admission == Admission::Ready ? std::optional<nr::FrameKey>{{f.instance, 1, f.ingress}} : std::nullopt,
            std::nullopt, false};
        f.ticks.latch(tick, selected);
    }
    if (!tick) f.admission = Admission::ClockUnavailable;
    if ((action != nr::TickAction::CaptureOnce && action != nr::TickAction::ReuseLatch) || f.admission != Admission::Ready) {
        bypass(f);
        return;
    }
    auto *texture = gs_texrender_get_texture(f.capture);
    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
    // The private capture is premultiplied; OBS's normal scene blend expects straight RGB.
    while (gs_effect_loop(effect, "DrawAlphaDivide")) gs_draw_sprite(texture, 0, f.width, f.height);
    gs_enable_framebuffer_srgb(srgb);
}
const char *name(void *) { return "Native NR (sampler only)"; }
}
extern "C" void nr_register_obs_filter(void)
{
    obs_source_info info{};
    info.id = "susemi_nr_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = name;
    info.create = create;
    info.destroy = destroy;
    info.get_properties = properties;
    info.video_render = render;
    obs_register_source(&info);
}
