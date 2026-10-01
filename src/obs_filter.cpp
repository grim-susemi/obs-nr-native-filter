// SPDX-License-Identifier: GPL-3.0-only
#include "obs_filter.h"
#include "obs_native_output.h"
#include <obs-module.h>
#include <graphics/vec4.h>
#include <atomic>
#include <fstream>
#include <stdexcept>

namespace {
using nr::sampler::Admission;
std::atomic<uint64_t> nextInstance{1};
struct Filter {
    obs_source_t *source;
    const uint64_t instance;
    nr::PerInstanceTick ticks;
    gs_texrender_t *capture = nullptr;
    gs_texture_t *output = nullptr;
    std::unique_ptr<nr::obs_adapter::NativeOutput> native;
    nr::model::Config config;
    bool stopping = false, nativeFailed = false, deterministicQa = false;
    gs_color_space color = GS_CS_SRGB;
    uint64_t ingress = 0, clock = 0, interval = 0;
    uint32_t width = 0, height = 0;
    std::atomic<Admission> admission{Admission::NoTarget};
    explicit Filter(obs_source_t *s) : source(s), instance(nextInstance.fetch_add(1)), ticks(instance) {}
};
std::vector<std::unique_ptr<nr::obs_adapter::NativeOutput>> retiring;
std::vector<std::byte> bytes(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw std::runtime_error("native filter shader unavailable");
    const auto length=file.tellg();
    if(length<=0) throw std::runtime_error("native filter shader empty");
    std::vector<std::byte> result(static_cast<size_t>(length));file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()),length);
    if(!file) throw std::runtime_error("native filter shader read failed");
    return result;
}
void retire(void*,uint32_t,uint32_t)
{
    retiring.erase(std::remove_if(retiring.begin(),retiring.end(),[](const auto& output) {
        return output->try_close();
    }),retiring.end());
}
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
const char *status(const Filter& f)
{
    if(f.nativeFailed) return "Error: native processing failed; original source shown";
    if(!f.native || f.admission!=Admission::Ready) return status(f.admission);
    if(f.stopping) return f.native->closed()?"Stopped: resources retired":"Stopping: physical resources retained";
    const auto* selected=f.ticks.peek(nr::sampler::canonical_tick(f.clock,f.interval));
    if(selected && selected->status==nr::Status::Applied)
        return f.deterministicQa?"QA transport only: exact prior sample":"Applied: native NR exact prior sample";
    return selected && selected->kind==nr::OutputKind::DelayedRaw?
        "Bypassed: untouched prior sample; processing not ready":"Priming: current raw sample";
}
void native_snapshot(void* opaque,calldata_t* data)
{
    auto& f=*static_cast<Filter*>(opaque);
    if(auto* job=static_cast<std::shared_ptr<nr::model::Job>*>(calldata_ptr(data,"retain_job")))
        *job=f.native?f.native->current_job():nullptr;
    if(auto* context=static_cast<std::shared_ptr<nr::interop::Context>*>(calldata_ptr(data,"retain_context")))
        *context=f.native?f.native->context():nullptr;
    if(auto* job=static_cast<std::shared_ptr<nr::model::Job>*>(calldata_ptr(data,"retain_selected_job")))
        *job=f.native?f.native->selected_job():nullptr;
    if(auto* raw=static_cast<std::shared_ptr<const nr::obs_adapter::Capture>*>(calldata_ptr(data,"retain_raw")))
        *raw=f.native?f.native->selected_raw():nullptr;
    if(auto* bridge=static_cast<std::shared_ptr<nr::interop::Bridge>*>(calldata_ptr(data,"retain_bridge")))
        *bridge=f.native?f.native->transport():nullptr;
    if(auto* exited=static_cast<std::shared_future<void>*>(calldata_ptr(data,"retain_exit")))
        *exited=f.native?f.native->worker_exit():std::shared_future<void>{};
    calldata_set_ptr(data,"output_texture",f.output);
    calldata_set_string(data,"status",status(f));
    calldata_set_bool(data,"deterministic",f.deterministicQa);
    calldata_set_bool(data,"closed",!f.native || f.native->closed());
    calldata_set_bool(data,"failed",f.nativeFailed);
    calldata_set_int(data,"retiring",static_cast<int64_t>(retiring.size()));
    const auto* selection=f.ticks.peek(nr::sampler::canonical_tick(f.clock,f.interval));
    calldata_set_int(data,"kind",selection?static_cast<int64_t>(selection->kind):0);
}
void native_stop(void* opaque,calldata_t* data)
{
    auto& f=*static_cast<Filter*>(opaque);f.stopping=true;
    if(f.native) { f.native->request_stop();calldata_set_bool(data,"closed",f.native->try_close()); }
    else calldata_set_bool(data,"closed",true);
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
    calldata_set_string(data, "status", status(f));
    const auto *selection = f.ticks.peek(nr::sampler::canonical_tick(f.clock, f.interval));
    calldata_set_int(data, "selected", selection && selection->frame ? selection->frame->ingressSequence : 0);
    calldata_set_int(data, "epoch", selection && selection->frame ? selection->frame->contentEpoch : 0);
    calldata_set_string(data, "color", f.admission == Admission::Ready ?
        (f.color==GS_CS_709_SCRGB?"ScRgbLinear":"Rec709Srgb") : "Unspecified");
    calldata_set_string(data, "alpha", f.admission == Admission::Ready ? "Premultiplied" : "Unspecified");
}
void *create(obs_data_t *settings, obs_source_t *source)
{
    auto *f = new Filter(source);
    proc_handler_add(obs_source_get_proc_handler(source),
        "void nr_sampler_snapshot(out int instance, out int tick, out int clock, out int interval, out int ingress, out int selected, out int epoch, out int width, out int height, out int admission, out string status, out string color, out string alpha)", snapshot, f);
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_native_snapshot(in ptr retain_job, in ptr retain_selected_job, in ptr retain_raw, in ptr retain_context, in ptr retain_bridge, in ptr retain_exit, out ptr output_texture, out bool closed, out bool failed, out int retiring, out int kind, out bool deterministic, out string status)",native_snapshot,f);
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_native_stop(out bool closed)",native_stop,f);
    const std::filesystem::path shaderRoot(obs_data_get_string(settings,"nr_shader_root"));
    if(!shaderRoot.empty()) {
        try {
            f->deterministicQa=obs_data_get_bool(settings,"nr_qa_deterministic");
            f->config.runtime={std::filesystem::canonical(L"C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll"),
                "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a"};
            f->config.driver=std::filesystem::canonical(L"C:/Windows/System32/DriverStore/FileRepository/nv_dispi.inf_amd64_b20cc8aeaed64fc2/_nvngx.dll");
            f->config.data=std::filesystem::canonical(obs_data_get_string(settings,"nr_data_root"));
            f->config.codec=bytes(shaderRoot/"nr_codec.dxil");f->config.color=bytes(shaderRoot/"nr_color.dxil");
            f->config.guides=bytes(shaderRoot/"nr_guides.cso");
            f->native=std::make_unique<nr::obs_adapter::NativeOutput>(f->instance,f->config,
                bytes(shaderRoot/"nr_interop_vertex.cso"),bytes(shaderRoot/"nr_interop_pixel.cso"),
                bytes(shaderRoot/"nr_obs_analysis.cso"),f->deterministicQa);
        } catch(const std::exception&) { f->nativeFailed=true; }
    }
    return f;
}
void destroy(void *opaque)
{
    auto *f = static_cast<Filter *>(opaque);
    obs_enter_graphics();
    if(f->native) {
        f->native->request_stop();
        if(!f->native->try_close()) retiring.push_back(std::move(f->native));
    }
    gs_texture_destroy(f->output);
    gs_texrender_destroy(f->capture);
    obs_leave_graphics();
    delete f;
}
obs_properties_t *properties(void *opaque)
{
    auto *props = obs_properties_create();
    // The standard OBS filter checkbox controls enablement. Full controls are Task15.
    obs_properties_add_text(props, "sampler_status", opaque?status(*static_cast<Filter *>(opaque)):status(Admission::NoTarget), OBS_TEXT_INFO);
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
    if(f.stopping) {
        if(f.native) (void)f.native->try_close();
        bypass(f);return;
    }
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
        const gs_color_space preferred[]{GS_CS_709_SCRGB,GS_CS_SRGB};
        const auto space = target ? obs_source_get_color_space(target, 2, preferred) : GS_CS_SRGB;
        f.color=space;
        f.admission = nr::sampler::admit(target && obs_filter_get_parent(f.source),
            gs_get_device_type() == GS_DEVICE_DIRECT3D_11, f.width, f.height,
            space == GS_CS_SRGB || (f.native && space == GS_CS_709_SCRGB));
        if (f.admission == Admission::Ready) {
            if (!f.capture) f.capture = gs_texrender_create(space==GS_CS_SRGB?GS_RGBA:GS_RGBA16F, GS_ZS_NONE);
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
        nr::Selection selected{tick,
            f.admission == Admission::Ready ? nr::OutputKind::PrimingRaw : nr::OutputKind::CurrentSource,
            nr::Status::Bypassed, nr::Reason::None,
            f.admission == Admission::Ready ? std::optional<nr::FrameKey>{{f.instance, 1, f.ingress}} : std::nullopt,
            std::nullopt, false};
        if(f.native && f.admission==Admission::Ready && !f.nativeFailed) {
            try {
                const bool hdr=space==GS_CS_709_SCRGB;
                if(!f.output) f.output=gs_texture_create(f.width,f.height,hdr?GS_RGBA16F:GS_RGBA_UNORM,1,nullptr,0);
                if(!f.output) throw std::runtime_error("native filter output texture unavailable");
                const auto epochs=f.native->epochs();
                const nr::IngressSample input{{f.instance,epochs.contentEpoch,f.ingress},tick,tick-1,
                    reinterpret_cast<uintptr_t>(target),reinterpret_cast<uintptr_t>(gs_get_device_obj()),
                    epochs.resourceGeneration,nr::SourceValidity::Valid,
                    {{f.width,f.height},{0,0,f.width,f.height},hdr?nr::PixelFormat::Rgba16Float:nr::PixelFormat::Rgba8,
                        hdr?nr::ColorSpace::ScRgbLinear:nr::ColorSpace::Rec709Srgb,nr::AlphaMode::Premultiplied,80},
                    {{1},true,f.config.runtime.canonicalPath.generic_string(),{1},nr::core_default_tuning()}};
                const auto result=f.native->tick(input,
                    static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_texrender_get_texture(f.capture))),
                    static_cast<ID3D11Texture2D*>(gs_texture_get_obj(f.output)));
                std::destroy_at(&selected);std::construct_at(&selected,result);
            } catch(const std::exception&) { f.nativeFailed=true; }
        }
        f.ticks.latch(tick, selected);
    }
    if (!tick) f.admission = Admission::ClockUnavailable;
    if ((action != nr::TickAction::CaptureOnce && action != nr::TickAction::ReuseLatch) || f.admission != Admission::Ready) {
        bypass(f);
        return;
    }
    auto *texture = f.native && !f.nativeFailed?f.output:gs_texrender_get_texture(f.capture);
    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
    // The private capture is premultiplied; OBS's normal scene blend expects straight RGB.
    while (gs_effect_loop(effect, "DrawAlphaDivide")) gs_draw_sprite(texture, 0, f.width, f.height);
    gs_enable_framebuffer_srgb(srgb);
}
gs_color_space color_space(void* opaque,size_t count,const gs_color_space* preferred)
{
    auto& f=*static_cast<Filter*>(opaque);
    auto* target=obs_filter_get_target(f.source);
    return target?obs_source_get_color_space(target,count,preferred):GS_CS_SRGB;
}
const char *name(void *) { return "Native NR"; }
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
    info.video_get_color_space = color_space;
    obs_register_source(&info);
    obs_add_main_render_callback(retire,nullptr);
}
