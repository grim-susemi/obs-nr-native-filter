// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include "host_log_boundary.h"
#include "nr_color_scenario.h"
#include "nr_model_scenario.h"
#include "nr_model_ownership.h"
#include "nr_bootstrap_scenario.h"
#include "nr_bootstrap_images.h"
#include "recording_scenario.h"
#include "nr_recording_encode.h"
#include "nr_model.h"
#include "obs_native_output.h"
#include "obs_filter.h"
#include "nr_ghost_scenario.h"
#include "nr_slot_scenario.h"
#include "nr_instance_scenario.h"
#include "nr_instance_json.h"
#include <obs.h>
#include "nr_interop_scenario.h"
#include "nr_flow_scenario.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <array>
#include <vector>
#include <stdexcept>
#include <graphics/vec4.h>
#include <cstring>
#include <sstream>
#include <syncstream>


namespace {

// Finite QA source. Production never maps textures or reads pixels back.
struct Fixture {
    gs_texture_t *texture = nullptr;
    unsigned renders = 0;
    uint32_t extent = 64;
    gs_color_space space = GS_CS_SRGB;
    bool lifecycle=false;
    int validity=1;
    uint64_t cut=0;
    uint32_t content=1;
};
Fixture *fixture = nullptr;
std::array<uint8_t, 4> pixel(uint64_t clock, unsigned x, unsigned y)
{
    const uint8_t alpha[]{0, 64, 128, 255};
    const auto a = alpha[(y / 16) % 4];
    const auto id = static_cast<unsigned>((clock / obs_get_frame_interval_ns()) % 31);
    return {static_cast<uint8_t>((id + x % 7) * a / 64),
            static_cast<uint8_t>((8 + y % 9) * a / 64),
            static_cast<uint8_t>((4 + x % 5) * a / 64), a};
}
void fixture_lifecycle(void* opaque,calldata_t* data)
{
    const auto& f=*static_cast<Fixture*>(opaque);
    calldata_set_int(data,"validity",f.validity);calldata_set_int(data,"cut",f.cut);
}
void *fixture_create(obs_data_t *settings, obs_source_t *source)
{
    fixture = new Fixture;
    fixture->lifecycle=obs_data_get_bool(settings,"nr_lifecycle_fixture");
    if(fixture->lifecycle) {
        fixture->extent=1920;
        proc_handler_add(obs_source_get_proc_handler(source),"void nr_source_lifecycle(out int validity, out int cut)",
            fixture_lifecycle,fixture);
    }
    return fixture;
}
void fixture_destroy(void *data)
{
    auto *f = static_cast<Fixture *>(data);
    obs_enter_graphics(); gs_texture_destroy(f->texture); obs_leave_graphics();
    delete f; if(fixture==f)fixture = nullptr;
}
uint32_t fixture_extent(void *data) { return static_cast<Fixture *>(data)->extent; }
uint32_t fixture_height(void *data) { return static_cast<Fixture *>(data)->lifecycle?1080:fixture_extent(data); }
gs_color_space fixture_space(void *data, size_t, const gs_color_space *) { return static_cast<Fixture *>(data)->space; }
const char *fixture_name(void *) { return "Task6 finite alpha/content fixture"; }
void fixture_render(void *data, gs_effect_t *)
{
    auto &f = *static_cast<Fixture *>(data);
    if (!f.extent) return;
    ++f.renders;
    if(f.lifecycle) {
        const bool hdr=f.space==GS_CS_709_SCRGB;
        const auto format=hdr?GS_RGBA16F:GS_RGBA;
        if(f.texture && gs_texture_get_color_format(f.texture)!=format) {
            gs_texture_destroy(f.texture);f.texture=nullptr;
        }
        if(!f.texture)f.texture=gs_texture_create(1920,1080,format,1,nullptr,GS_DYNAMIC);
        const auto image=nr::qa::bootstrap::fixture_bytes(f.content,hdr);
        gs_texture_set_image(f.texture,image.data(),1920*(hdr?8:4),false);
        const bool srgb=gs_framebuffer_srgb_enabled();gs_enable_framebuffer_srgb(false);
        auto* effect=obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_effect_set_texture(gs_effect_get_param_by_name(effect,"image"),f.texture);
        while(gs_effect_loop(effect,"DrawAlphaDivide"))gs_draw_sprite(f.texture,0,f.extent,1080);
        gs_enable_framebuffer_srgb(srgb);
        return;
    }
    std::array<uint8_t, 64 * 64 * 4> bytes{};
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        const auto p = pixel(obs_get_video_frame_time(), x, y);
        for (unsigned c = 0; c < 4; ++c) bytes[(y * 64 + x) * 4 + c] = p[c];
    }
    if (!f.texture) f.texture = gs_texture_create(64, 64, GS_RGBA, 1, nullptr, GS_DYNAMIC);
    gs_texture_set_image(f.texture, bytes.data(), 64 * 4, false);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    const bool srgb = gs_framebuffer_srgb_enabled(); gs_enable_framebuffer_srgb(false);
    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), f.texture);
    while (gs_effect_loop(effect, "Draw")) gs_draw_sprite(f.texture, 0, 64, 64);
    gs_enable_framebuffer_srgb(srgb); gs_blend_state_pop();
}
struct Sampler {
    HANDLE complete = nullptr;
    obs_source_t *source = nullptr, *filter = nullptr, *unattached = nullptr;
    obs_scene_t *scene = nullptr;
    std::array<obs_view_t *, 3> views{};
    unsigned step = 0;
    uint64_t previousIngress = 0, previousTick = 0;
    bool failed = false;
    ~Sampler()
    {
        for (auto *view : views) if (view) obs_view_destroy(view);
        if (scene) obs_scene_release(scene);
        if (source && filter) obs_source_filter_remove(source, filter);
        obs_source_release(filter); obs_source_release(unattached); obs_source_release(source);
        if (complete) CloseHandle(complete);
    }
};
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
// Returns the actual GPU pixels; this is a bounded QA-only readback.
struct PixelProof {
    unsigned errors = 0;
    bool stateRestored = true;
    uint64_t actualHash = 14695981039346656037ull, expectedHash = 14695981039346656037ull;
    std::array<unsigned, 4> alpha{};
};
PixelProof render_view(obs_view_t *view, obs_source_t *direct, unsigned size, bool empty)
{
    auto *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    auto *stage = gs_stagesurface_create(size, size, GS_RGBA);
    PixelProof proof;
    if (!target || !stage || !gs_texrender_begin(target, size, size)) {
        gs_stagesurface_destroy(stage); gs_texrender_destroy(target);
        throw std::runtime_error("QA texrender admission failed");
    }
    gs_blend_state_push();
    const bool srgb = gs_framebuffer_srgb_enabled(); gs_enable_framebuffer_srgb(false);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    vec4 clear{}; gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
    gs_ortho(0, static_cast<float>(size), 0, static_cast<float>(size), -100, 100);
    gs_rect beforeViewport{}, afterViewport{};
    gs_get_viewport(&beforeViewport);
    const auto beforeTarget = gs_get_render_target();
    const auto beforeSpace = gs_get_color_space();
    const bool beforeLinear = gs_get_linear_srgb();
    if (view) obs_view_render(view); else obs_source_video_render(direct);
    gs_get_viewport(&afterViewport);
    proof.stateRestored = beforeTarget == gs_get_render_target() && beforeSpace == gs_get_color_space() &&
        beforeLinear == gs_get_linear_srgb() && !gs_framebuffer_srgb_enabled() &&
        std::memcmp(&beforeViewport, &afterViewport, sizeof(beforeViewport)) == 0;
    gs_texrender_end(target);
    gs_enable_framebuffer_srgb(srgb); gs_blend_state_pop();
    gs_stage_texture(stage, gs_texrender_get_texture(target));
    uint8_t *mapped = nullptr; uint32_t stride = 0;
    if (!gs_stagesurface_map(stage, &mapped, &stride)) {
        gs_stagesurface_destroy(stage); gs_texrender_destroy(target);
        throw std::runtime_error("QA readback map failed");
    }
    // View-only extent changes never change the source's 64x64 pixel coordinates.
    for (unsigned y = 0; y < size; ++y) for (unsigned x = 0; x < size; ++x) {
        const auto expected = empty || x >= 64 || y >= 64 ? std::array<uint8_t, 4>{} : pixel(obs_get_video_frame_time(), x, y);
        for (unsigned c = 0; c < 4; ++c) {
            const auto actual = mapped[y * stride + x * 4 + c];
            if (std::abs(static_cast<int>(actual) - expected[c]) > 1) ++proof.errors;
            proof.actualHash = (proof.actualHash ^ actual) * 1099511628211ull;
            proof.expectedHash = (proof.expectedHash ^ expected[c]) * 1099511628211ull;
        }
    }
    for (unsigned band = 0; band < 4; ++band) proof.alpha[band] = mapped[(band * 16 + 8) * stride + 8 * 4 + 3];
    gs_stagesurface_unmap(stage);
    gs_stagesurface_destroy(stage); gs_texrender_destroy(target);
    return proof;
}
void sampler_draw(void *opaque, uint32_t, uint32_t)
{
    auto &s = *static_cast<Sampler *>(opaque);
    if (s.step >= 8) return;
    try {
        fixture->extent = s.step == 4 ? 0 : 64;
        fixture->space = s.step == 5 ? GS_CS_709_EXTENDED : GS_CS_SRGB;
        const char *names[]{"preview", "program", "projector"};
        uint64_t ingress = 0, tick = 0;
        const unsigned rendersBefore = fixture->renders;
        for (unsigned draw = 0; draw < (s.step == 6 ? 1u : 6u); ++draw) {
            const unsigned view = (draw + s.step) % 3;
            const unsigned size = view == 2 && draw >= 3 ? 128 : 64;
            auto proof = render_view(s.step == 6 ? nullptr : s.views[view], s.unattached, size, s.step == 4 || s.step == 6);
            if (s.step == 5) {
                // Extended-color bypass must match OBS's actual OFF conversion, not an SDR oracle.
                const auto beforeOff = fixture->renders;
                obs_source_set_enabled(s.filter, false);
                const auto off = render_view(s.views[view], nullptr, size, false);
                obs_source_set_enabled(s.filter, true);
                require(fixture->renders > beforeOff, "OFF baseline reused filtered pixels");
                proof.errors = proof.actualHash == off.actualHash && proof.alpha == off.alpha ? 0 : 1;
                proof.expectedHash = off.actualHash;
                proof.stateRestored = proof.stateRestored && off.stateRestored;
            }
            calldata_t data; calldata_init(&data);
            const bool queried = proc_handler_call(obs_source_get_proc_handler(s.step == 6 ? s.unattached : s.filter), "nr_sampler_snapshot", &data);
            if (!queried) { calldata_free(&data); throw std::runtime_error("production snapshot missing"); }
            const auto currentIngress = static_cast<uint64_t>(calldata_int(&data, "ingress"));
            const auto currentTick = static_cast<uint64_t>(calldata_int(&data, "tick"));
            const auto admission = calldata_int(&data, "admission");
            std::cout << "NR_SAMPLER_VIEW {\"step\":" << s.step << ",\"view\":\"" << (s.step == 6 ? "no-target" : names[view])
                      << "\",\"draw\":" << draw << ",\"view_extent\":" << size
                      << ",\"instance\":" << calldata_int(&data, "instance") << ",\"epoch\":" << calldata_int(&data, "epoch")
                      << ",\"tick\":" << currentTick << ",\"clock\":" << calldata_int(&data, "clock")
                      << ",\"interval\":" << calldata_int(&data, "interval") << ",\"ingress\":" << currentIngress
                      << ",\"selected\":" << calldata_int(&data, "selected") << ",\"admission\":" << admission
                      << ",\"pixel_errors\":" << proof.errors << ",\"channels_checked\":" << size * size * 4
                      << ",\"state_restored\":" << (proof.stateRestored ? "true" : "false")
                      << ",\"renderer\":\"" << gs_get_device_name() << "\",\"oracle\":\"" << (s.step == 5 ? "same-tick-source-OFF" : "analytic-source") << "\""
                      << ",\"actual_fnv1a64\":\"" << proof.actualHash << "\",\"expected_fnv1a64\":\"" << proof.expectedHash
                      << "\",\"alpha_samples\":[" << proof.alpha[0] << ',' << proof.alpha[1] << ',' << proof.alpha[2] << ',' << proof.alpha[3]
                      << "],\"source_render_count\":" << fixture->renders << ",\"color\":\"" << calldata_string(&data, "color") << "\",\"alpha\":\"" << calldata_string(&data, "alpha") << "\""
                      << ",\"status\":\"" << calldata_string(&data, "status") << "\"}\n" << std::flush;
            const bool selected = calldata_int(&data, "selected") == static_cast<int64_t>(currentIngress);
            calldata_free(&data);
            require(proof.errors == 0, "pixel/alpha mismatch");
            require(proof.stateRestored, "graphics state not restored");
            if (draw) require(currentIngress == ingress && currentTick == tick, "view draw reallocated ingress or changed latch");
            ingress = currentIngress; tick = currentTick;
            if (s.step < 4) require(admission == 0 && selected, "supported sample not selected");
            if (s.step == 4) require(admission == 2 && ingress == s.previousIngress, "zero extent not bypassed");
            if (s.step == 5) require(admission == 4 && ingress == s.previousIngress, "unknown color not bypassed");
            if (s.step == 6) require(admission == 1 && ingress == 0, "unattached filter not bypassed");
            if (s.step == 7) require(admission == 3 && ingress == 0 && gs_get_device_type() == GS_DEVICE_OPENGL, "unsupported renderer did not preserve original source");
        }
        if (s.step < 4) {
            require(ingress == s.previousIngress + 1 && tick > s.previousTick, "canonical ingress did not advance once");
            require(fixture->renders - rendersBefore == 1, "target source captured more than once per tick");
            s.previousIngress = ingress; s.previousTick = tick;
        }
        s.step = s.step == 6 ? 8 : s.step + 1;
    } catch (const std::exception &e) {
        std::cout << "NR_SAMPLER_FAILURE " << e.what() << "\n" << std::flush;
        s.failed = true; s.step = 8;
    }
    if (s.step == 8) SetEvent(s.complete);
}
bool run_sampler(bool unsupported = false)
{
    Sampler s; // Cleanup scope exists before event/source/view creation.
    if (unsupported) s.step = 7;
    s.complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.complete) return false;
    obs_source_info info{};
    info.id = "nr_task6_fixture"; info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = fixture_name; info.create = fixture_create; info.destroy = fixture_destroy;
    info.get_width = fixture_extent; info.get_height = fixture_extent;
    info.video_render = fixture_render; info.video_get_color_space = fixture_space;
    obs_register_source(&info);
    s.source = obs_source_create_private("nr_task6_fixture", "Task6 fixture", nullptr);
    s.filter = obs_source_create_private("susemi_nr_filter", "Task6 native sampler", nullptr);
    s.unattached = obs_source_create_private("susemi_nr_filter", "Task6 unattached", nullptr);
    if (!s.source || !s.filter || !s.unattached) return false;
    require(obs_source_get_type(s.filter) == OBS_SOURCE_TYPE_FILTER, "native filter type incorrect");
    obs_source_filter_add(s.source, s.filter);
    s.scene = obs_scene_create_private("Task6 sampler scene");
    if (!s.scene || !obs_scene_add(s.scene, s.source)) return false;
    for (auto &view : s.views) {
        view = obs_view_create(); if (!view) return false;
        obs_view_set_source(view, 0, obs_scene_get_source(s.scene));
    }
    obs_enter_graphics();
    std::cout << "NR_SAMPLER_REGISTER source=nr_task6_fixture filter=susemi_nr_filter type=filter scene=Task6_sampler_scene renderer=" << gs_get_device_name() << "\n" << std::flush;
    obs_leave_graphics();
    obs_add_main_render_callback(sampler_draw, &s);
    const auto wait = WaitForSingleObject(s.complete, 15000);
    obs_remove_main_render_callback(sampler_draw, &s);
    std::cout << "NR_SAMPLER_SCOPE NR=not_loaded recording=not_tested HDR=not_claimed\n" << std::flush;
    return wait == WAIT_OBJECT_0 && !s.failed && s.step == 8;
}
struct Lifecycle {
    Sampler scope;
    std::filesystem::path root;
    unsigned step=0,frames=0,applied=0,ordinary=0,resets=0,evaluations=0;
    const bool native;
    bool failed=false,stopping=false;
    uint64_t lastInstance=0,lastEpoch=0;
    nr::lifecycle::ReplacementBaseline replacement;
    uint64_t requiredSettingsRevision=0;
    uint64_t disabledInstance=0,disabledContent=0,disabledHistory=0,disabledGeneration=0;
    std::shared_ptr<nr::model::QaSignal> exited;
    struct Identity { uint64_t ingress=0,hash=0;uint32_t content=0; };
    std::array<Identity,2> images{};
    explicit Lifecycle(const std::filesystem::path& value,bool actual):root(value),native(actual) {}
};
obs_source_t* lifecycle_filter(const std::filesystem::path& root,bool native)
{
    auto* settings=obs_data_create();
    obs_data_set_string(settings,"nr_shader_root",(root/"bin/64bit").string().c_str());
    obs_data_set_string(settings,"nr_data_root",(root/"profile/ngx-data").string().c_str());
    obs_data_set_bool(settings,"nr_qa_deterministic",!native);
    auto* filter=obs_source_create_private("susemi_nr_filter","Task13 lifecycle",settings);
    obs_data_release(settings);return filter;
}
void lifecycle_vendor_trace(const nr::lifecycle::VendorSnapshot& trace)
{
    using namespace nr::lifecycle;
    std::cout<<"{\"owner\":"<<trace.owner<<",\"instance\":"<<trace.instance
        <<",\"generation\":"<<trace.generation<<",\"device\":"<<trace.device<<",\"sequence\":"<<trace.sequence
        <<",\"coherent\":"<<(trace.coherent?"true":"false")<<",\"nested_core_observed\":false,\"calls\":[";
    for(size_t i=0;i<trace.calls.size();++i) {
        if(i)std::cout<<',';
        const auto stage=static_cast<VendorStage>(i);const auto& calls=trace.calls[i];
        std::cout<<"{\"stage\":"<<i<<",\"return_kind\":\""
            <<(vendor_return_kind(stage)==VendorReturnKind::SdkResult?"SdkResult":"Win32Bool")
            <<"\",\"count\":"<<calls.count;
        const std::array<const VendorCall*,2> values{&calls.first,&calls.latest};
        for(size_t n=0;n<values.size();++n) {
            const auto& c=*values[n];
            std::cout<<",\""<<(n?"latest":"first")<<"\":{\"enter\":"<<c.enter<<",\"returned\":"<<c.returned
                <<",\"result_known\":"<<(c.returned?"true":"false")<<",\"result\":"<<c.result
                <<",\"remaining_known\":"<<(c.returned && stage==VendorStage::CoreShutdown?"true":"false")
                <<",\"remaining\":"<<c.remaining<<",\"error\":"<<c.error
                <<",\"module\":"<<c.module<<",\"procedure\":"<<c.procedure<<",\"thread\":"<<c.thread<<'}';
        }
        std::cout<<'}';
    }
    std::cout<<"]}";
}
void lifecycle_frame_key(const nr::FrameKey* key)
{
    if(!key){std::cout<<"null";return;}
    std::cout<<"{\"instance\":"<<key->instanceId<<",\"content_epoch\":"<<key->contentEpoch
        <<",\"ingress\":"<<key->ingressSequence<<'}';
}
void lifecycle_write_closure(const nr::lifecycle::CloseSnapshot& d,const char* stage,uint64_t request)
{
    std::cout<<"NR_LIFECYCLE_CLOSURE {\"receipt_schema\":2,\"stage\":\""<<stage<<"\",\"request\":"<<request<<",\"instance\":"<<d.instance
        <<",\"sequence\":"<<d.sequence<<",\"block\":"<<static_cast<uint32_t>(d.block)
        <<",\"refusing_generation\":"<<d.refusingGeneration<<",\"refusing_serial\":"<<d.refusingSerial
        <<",\"refusing_slot\":"<<d.refusingSlot<<",\"normal_closed\":"<<(nr::lifecycle::close_complete(d)?"true":"false")
        <<",\"source_closed\":"<<(nr::lifecycle::close_complete(d)?"true":"false")
        <<",\"owner\":"<<d.owner<<",\"caller\":"<<d.caller<<",\"tick\":"<<d.tick<<",\"last_tick\":"<<d.lastTick
        <<",\"stop\":"<<(d.stop?"true":"false")<<",\"worker_exit\":"<<(d.workerExited?"true":"false")
        <<",\"source_detached\":"<<(d.sourceDetached?"true":"false")<<",\"closed\":"<<(d.closed?"true":"false")
        <<",\"pipeline\":"<<static_cast<uint32_t>(d.pipeline)<<",\"generations\":[";
    for(size_t i=0;i<d.generations.size();++i) {
        const auto& g=d.generations[i];if(i)std::cout<<',';
        std::cout<<"{\"id\":"<<g.id<<",\"graphics\":"<<g.graphics<<",\"graphics_done\":"<<g.graphicsDone
            <<",\"cutoff_leases\":"<<g.cutoffLeases<<",\"requested\":"<<(g.requested?"true":"false")
            <<",\"closed\":"<<(g.closed?"true":"false")<<",\"quiescent\":"<<(g.quiescent?"true":"false")
            <<",\"has_model\":"<<(g.hasModel?"true":"false")
            <<",\"bridge\":"<<static_cast<uint32_t>(g.bridge)<<",\"generation\":"<<static_cast<uint32_t>(g.generation)
            <<",\"model_settlement\":"<<static_cast<uint32_t>(g.model.settlement)
            <<",\"feature_release\":"<<static_cast<uint32_t>(g.model.feature)<<",\"ofa_release\":"<<static_cast<uint32_t>(g.model.ofa)
            <<",\"module_release\":"<<static_cast<uint32_t>(g.model.module)<<",\"core_close\":"<<static_cast<uint32_t>(g.model.core)
            <<",\"core_result\":"<<g.model.coreResult<<",\"core_remaining\":"<<g.model.coreRemaining
            <<",\"module_error\":"<<g.model.moduleError<<",\"exception\":"<<g.model.exception
            <<",\"model_attempts\":"<<g.model.attempts<<",\"coherent\":"<<(g.model.coherent?"true":"false")
            <<",\"pending_serial\":"<<g.model.pendingSerial<<",\"model_completion_required\":"<<g.model.completionRequired
            <<",\"model_completion_done\":"<<g.model.completionDone
            <<",\"callback_retained\":"<<(g.model.callbackRetained?"true":"false")
            <<",\"module_required\":"<<(g.model.moduleRequired?"true":"false")
            <<",\"module_present\":"<<(g.model.modulePresent?"true":"false")
            <<",\"sdk_disposition\":"<<static_cast<uint32_t>(g.model.sdk)<<",\"sdk_session_id\":"<<g.model.sessionId
            <<",\"sdk_owners_at_detachment\":"<<g.model.ownersAtDetachment<<",\"sdk_owners_remaining\":"<<g.model.owners_remaining()
            <<",\"sdk_owners_remaining_known\":"<<(g.model.ownersAtDetachment?"true":"false")
            <<",\"module_reference_released\":"<<(g.model.moduleReferenceReleased?"true":"false")
            <<",\"iat_restored\":"<<(g.model.iatRestored?"true":"false")
            <<",\"core_reference_released\":"<<(g.model.coreReferenceReleased?"true":"false")
            <<",\"sdk_retired\":"<<(nr::lifecycle::sdk_retirement_complete(g.model)?"true":"false")
            <<",\"image_unmapped\":false"
            <<",\"guide_pending\":"<<(g.model.guidePending?"true":"false")
            <<",\"core_called\":"<<(g.model.coreCalled?"true":"false")<<",\"vendor_trace\":";
        lifecycle_vendor_trace(g.model.vendor);
        std::cout<<",\"slots\":[";
        for(size_t slot=0;slot<g.slots.size();++slot) {
            const auto& s=g.slots[slot];if(slot)std::cout<<',';
            std::cout<<"{\"serial\":"<<s.serial<<",\"occupied\":"<<(s.occupied?"true":"false")
                <<",\"ready\":"<<(s.ready?"true":"false")<<",\"returned\":"<<(s.returned?"true":"false")
                <<",\"pin\":"<<(s.pin?"true":"false")<<",\"producer\":"<<s.producer<<",\"producer_done\":"<<s.producerDone
                <<",\"worker\":"<<s.worker<<",\"worker_done\":"<<s.workerDone
                <<",\"consumer\":"<<s.consumer<<",\"consumer_done\":"<<s.consumerDone<<'}';
        }
        std::cout<<"]}";
    }
    std::cout<<"],\"retiring_count\":"<<d.retiringCount<<"}\n"<<std::flush;
}
bool lifecycle_closure(const char* stage,bool retry=false,uint64_t request=0)
{
    nr::lifecycle::CloseSnapshot d{};
    calldata_t data;calldata_init(&data);calldata_set_ptr(&data,"snapshot",&d);
    calldata_set_bool(&data,"retry",retry);
    require(proc_handler_call(obs_get_proc_handler(),"nr_retirement_snapshot",&data),"surviving primitive closure diagnostic");
    calldata_free(&data);
    lifecycle_write_closure(d,stage,request);
    return d.workerExited && nr::lifecycle::close_complete(d);
}
struct RetainedRequest {
    HANDLE completed=nullptr;
    uint64_t sequence=0;
    bool normal=false;
};
bool instances_closed();
void instances_release();
void instances_request_stop();
bool instances_retained();
bool property_retained();
bool property_closed();
void property_request_stop();
bool property_release();
bool core_control_receipt(nr::qa::CoreControlOwner& owner,const char* stage,uint64_t request) {
    owner.refresh();const auto d=owner.snapshot();
    std::cout<<"NR_CORE_CONTROL {\"receipt_schema\":2,\"stage\":\""<<stage<<"\",\"request\":"<<request
        <<",\"owned_request\":"<<d.request<<",\"pending_request\":"<<d.pendingRequest
        <<",\"pending\":"<<(d.pending?"true":"false")<<",\"completed_steps\":"<<d.completedSteps
        <<",\"normal_closed\":"<<(d.closed?"true":"false")<<",\"callback_attached\":"<<(d.callbackAttached?"true":"false")
        <<",\"exception\":"<<(d.exception?"true":"false")<<",\"model_coherent\":"<<(d.model.coherent?"true":"false")
        <<",\"worker_attempts\":"<<d.model.attempts<<",\"core_result\":"<<d.model.coreResult
        <<",\"core_remaining\":"<<d.model.coreRemaining<<",\"module_error\":"<<d.model.moduleError
        <<",\"sdk_disposition\":"<<static_cast<uint32_t>(d.model.sdk)<<",\"sdk_session_id\":"<<d.model.sessionId
        <<",\"sdk_owners_at_detachment\":"<<d.model.ownersAtDetachment<<",\"sdk_owners_remaining\":"<<d.model.owners_remaining()
        <<",\"sdk_owners_remaining_known\":"<<(d.model.ownersAtDetachment?"true":"false")
        <<",\"module_reference_released\":"<<(d.model.moduleReferenceReleased?"true":"false")
        <<",\"iat_restored\":"<<(d.model.iatRestored?"true":"false")
        <<",\"core_reference_released\":"<<(d.model.coreReferenceReleased?"true":"false")
        <<",\"sdk_retired\":"<<(nr::lifecycle::sdk_retirement_complete(d.model)?"true":"false")
        <<",\"image_unmapped\":false"
        <<",\"vendor_trace\":";lifecycle_vendor_trace(d.model.vendor);std::cout<<"}"<<std::endl;
    return d.closed && !d.pending && !d.exception;
}
bool supervise_core_control(nr::qa::CoreControlOwner& owner,HANDLE request,HANDLE reply,HANDLE ready) {
    ResetEvent(reply);ResetEvent(ready);SetEvent(ready);
    uint64_t sequence=0;
    for(;;) {
        if(WaitForSingleObject(request,INFINITE)!=WAIT_OBJECT_0)return false;
        ResetEvent(request);ResetEvent(reply);++sequence;
        const bool admitted=owner.request(sequence+1);
        if(admitted)(void)owner.wait(15000);
        const bool normal=core_control_receipt(owner,"retained-request",sequence);
        std::cout<<"NR_QA_RETAINED_REPLY {\"request\":"<<sequence<<",\"normal_closed\":"
            <<(normal?"true":"false")<<",\"busy\":"<<(!admitted?"true":"false")<<"}"<<std::endl;
        SetEvent(reply);
        try{if(normal && owner.detach())return true;}
        catch(const std::exception& error){std::cout<<"NR_CORE_CONTROL_FAILURE "<<error.what()<<std::endl;}
    }
}
void retained_diagnose(void* opaque,uint32_t,uint32_t)
{
    auto& request=*static_cast<RetainedRequest*>(opaque);
    if(WaitForSingleObject(request.completed,0)==WAIT_OBJECT_0)return;
    try {
        if(property_retained()) {
            property_request_stop();
            request.normal=property_closed();
        } else {
            if(instances_retained())instances_request_stop();
            request.normal=instances_retained()?instances_closed():lifecycle_closure("retained-request",true,request.sequence);
        }
        std::cout<<"NR_QA_RETAINED_REPLY {\"request\":"<<request.sequence
            <<",\"normal_closed\":"<<(request.normal?"true":"false")<<"}\n"<<std::flush;
    } catch(const std::exception& error) {
        std::cout<<"NR_QA_RETAINED_DIAGNOSIS_FAILURE "<<error.what()<<std::endl;
    }
    SetEvent(request.completed);
}
bool supervise_retained(HANDLE requestEvent,HANDLE replyEvent,HANDLE readyEvent)
{
    ResetEvent(replyEvent);ResetEvent(readyEvent);
    SetEvent(readyEvent);
    uint64_t sequence=0;
    for(;;) {
        if(WaitForSingleObject(requestEvent,INFINITE)!=WAIT_OBJECT_0)return false;
        ResetEvent(requestEvent);ResetEvent(replyEvent);
        RetainedRequest request{CreateEventW(nullptr,TRUE,FALSE,nullptr),++sequence,false};
        if(!request.completed)return false;
        obs_add_main_render_callback(retained_diagnose,&request);
        const bool responded=WaitForSingleObject(request.completed,15000)==WAIT_OBJECT_0;
        obs_enter_graphics();
        obs_remove_main_render_callback(retained_diagnose,&request);
        obs_leave_graphics();
        CloseHandle(request.completed);
        if(!responded)std::cout<<"NR_QA_RETAINED_DIAGNOSIS_TIMEOUT request="<<sequence<<std::endl;
        SetEvent(replyEvent);
        if(responded && request.normal) {
            if(property_retained()) {if(property_release())return true;}
            else {instances_release();return true;}
        }
    }
}
uint32_t lifecycle_image_id(std::span<const uint8_t> bytes,bool hdr,uint32_t width)
{
    // Task12's two independent bit rows and moving landmark, sampled from the
    // actual full image. Resize changes source coordinates, not provenance.
    require(bytes.size()==static_cast<size_t>(width)*1080*(hdr?8:4),"lifecycle full image bounds");
    const auto mean=[&](uint32_t x,uint32_t y) {
        double sum=0;
        for(uint32_t dy=0;dy<8;++dy)for(uint32_t dx=0;dx<8;++dx) {
            const auto sx=(x+dx)*width/1920;
            const auto p=nr::qa::bootstrap::packed_rgb(bytes,static_cast<size_t>(y+dy)*width+sx,hdr,false);
            sum+=(p.r+p.g+p.b)/3;
        }
        return sum/64;
    };
    const auto high=mean(652,44),low=mean(716,44);
    require(high-low>.05,"lifecycle blank source image");
    const auto threshold=(high+low)/2;
    uint32_t id[2]{};
    for(unsigned row=0;row<2;++row)for(unsigned bit=0;bit<8;++bit)
        if(mean(44+bit*48,44+row*64)>threshold)id[row]|=1u<<bit;
    require(id[0]==id[1] && id[0]>0 && id[0]<=16 &&
        mean(804+id[0]*nr::qa::bootstrap::landmark_pitch,276)>threshold,"lifecycle source rows or moving landmark stale");
    return id[0];
}
std::vector<uint8_t> lifecycle_view(obs_view_t* view,bool hdr,uint32_t width)
{
    auto* target=gs_texrender_create(hdr?GS_RGBA16F:GS_RGBA,GS_ZS_NONE);
    require(target!=nullptr,"lifecycle full view target");
    try {
        require(gs_texrender_begin_with_color_space(target,width,1080,hdr?GS_CS_709_SCRGB:GS_CS_SRGB),
            "lifecycle full view begin");
        gs_blend_state_push();
        gs_blend_function_separate(GS_BLEND_SRCALPHA,GS_BLEND_INVSRCALPHA,GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
        vec4 clear{};gs_clear(GS_CLEAR_COLOR,&clear,0,0);
        gs_ortho(0,static_cast<float>(width),0,1080,-100,100);
        obs_view_render(view);gs_texrender_end(target);gs_blend_state_pop();
        const auto image=nr_bootstrap_read_texture(
            static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_texrender_get_texture(target))),hdr,width,1080);
        gs_texrender_destroy(target);return image;
    }catch(...) { gs_texrender_destroy(target);throw; }
}
void lifecycle_draw(void* opaque,uint32_t,uint32_t)
{
    auto& s=*static_cast<Lifecycle*>(opaque);
    try {
        if(s.stopping) {
            obs_source_video_render(s.scope.filter);
            calldata_t data;calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_stop",&data),
                "lifecycle graphics close receipt");
            const bool closed=calldata_bool(&data,"closed");calldata_free(&data);
            if(closed)SetEvent(s.scope.complete);
            return;
        }
        if(s.step>=15)return;
        const char* cases[]{"initial","identical-valid","explicit-cut","invalid","reconnect","unknown",
            "known-reconnect","HDR","SDR","resize","settings","disable","enable","remove-readd","new-instance"};
        if(s.frames==0) {
        if(s.step==2)++fixture->cut;
        if(s.step==3)fixture->validity=2;
        if(s.step==4)fixture->validity=1;
        if(s.step==5)fixture->validity=0;
        if(s.step==6)fixture->validity=1;
        if(s.step==7)fixture->space=GS_CS_709_SCRGB;
        if(s.step==8)fixture->space=GS_CS_SRGB;
        if(s.step==9)fixture->extent=2560;
        if(s.step==10) {
            calldata_t before;calldata_init(&before);
            require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_sampler_snapshot",&before),
                "settings pre-update revision observation");
            s.requiredSettingsRevision=static_cast<uint64_t>(calldata_int(&before,"settings_revision"))+1;
            calldata_free(&before);
            auto* settings=obs_source_get_settings(s.scope.filter);
            obs_data_set_double(settings,"nr_strength",.5);obs_source_update(s.scope.filter,settings);obs_data_release(settings);
        }
        if(s.step==11) {
            calldata_t before;calldata_init(&before);
            require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_sampler_snapshot",&before),
                "disabled pre-control lifecycle baseline");
            s.disabledInstance=static_cast<uint64_t>(calldata_int(&before,"instance"));
            s.disabledContent=static_cast<uint64_t>(calldata_int(&before,"content_epoch"));
            s.disabledHistory=static_cast<uint64_t>(calldata_int(&before,"history_epoch"));
            s.disabledGeneration=static_cast<uint64_t>(calldata_int(&before,"resource_generation"));
            calldata_free(&before);
            obs_source_set_enabled(s.scope.filter,false);
        }
        if(s.step==12)obs_source_set_enabled(s.scope.filter,true);
        if(s.step>=13) {
            obs_source_filter_remove(s.scope.source,s.scope.filter);
            if(s.step==14) {
                s.replacement.retire(s.lastInstance);
                obs_source_release(s.scope.filter);
                s.scope.filter=lifecycle_filter(s.root,s.native);
                require(s.scope.filter!=nullptr,"lifecycle re-add create");
            }
            obs_source_filter_add(s.scope.source,s.scope.filter);
        }
        // Repeating fixture content is intentional; transport validity is explicit.
        if(s.step!=1)++fixture->content;
        }
        const bool hdr=fixture->space==GS_CS_709_SCRGB;
        uint64_t selected=0,tick=0,instance=0,epoch=0,imageHash=0;
        uintptr_t baselineRawOwner=0,baselineCurrentOwner=0,baselineContextOwner=0;
        bool genuine=false,ordinary=false,cold=false,accepted=false;
        nr::lifecycle::HostLatch firstLatch;
        for(unsigned view=0;view<3;++view) {
            // Independent graphics capture happens with no retained Job owner.
            const auto viewed=lifecycle_view(s.scope.views[view],hdr,fixture->extent);
            const auto viewHash=nr::qa::bootstrap::hash(viewed);
            const auto viewId=lifecycle_image_id(viewed,hdr,fixture->extent);
            calldata_t data;calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_sampler_snapshot",&data),
                "lifecycle production snapshot");
            const auto now=static_cast<uint64_t>(calldata_int(&data,"selected"));
            const auto token=static_cast<uint64_t>(calldata_int(&data,"tick"));
            instance=static_cast<uint64_t>(calldata_int(&data,"instance"));
            epoch=static_cast<uint64_t>(calldata_int(&data,"content_epoch"));
            const auto observedKind=static_cast<nr::OutputKind>(calldata_int(&data,"kind"));
            const nr::lifecycle::HostLatch latch{instance,token,now,epoch,viewHash,observedKind};
            std::cout<<"NR_LIFECYCLE_LATCH {\"step\":"<<s.step<<",\"frame\":"<<s.frames<<",\"view\":"<<view
                <<",\"instance\":"<<instance<<",\"tick\":"<<token<<",\"selected\":"<<now<<",\"content_epoch\":"<<epoch
                <<",\"image_hash\":\""<<viewHash<<"\",\"baseline_raw_owner\":"<<baselineRawOwner
                <<",\"baseline_current_owner\":"<<baselineCurrentOwner<<",\"baseline_context_owner\":"<<baselineContextOwner
                <<",\"native_owner_observed\":false}"<<std::endl;
            if(view)require(latch==firstLatch,
                "lifecycle views changed immutable selection or pixels");
            else firstLatch=latch;
            selected=now;tick=token;
            if(view==0) {
                imageHash=viewHash;
                const bool placeholder=calldata_bool(&data,"placeholder");
                const auto ingress=static_cast<uint64_t>(calldata_int(&data,"ingress"));
                const auto kind=calldata_int(&data,"kind");
                uint64_t rawHash=0,currentHash=0,outputHash=0;
                uint64_t selectedSerial=0,selectedHistory=0;
                std::optional<uint64_t> selectedSettingsRevision;
                uint32_t rawId=0;
                std::shared_ptr<const nr::obs_adapter::Capture> raw,current;
                std::shared_ptr<nr::model::Job> job;
                std::shared_ptr<nr::interop::Context> context;
                std::shared_ptr<nr::interop::Bridge> bridge;
                calldata_t native;calldata_init(&native);
                calldata_set_ptr(&native,"retain_raw",&raw);calldata_set_ptr(&native,"retain_current_raw",&current);
                calldata_set_ptr(&native,"retain_selected_job",&job);calldata_set_ptr(&native,"retain_context",&context);
                calldata_set_ptr(&native,"retain_bridge",&bridge);
                require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_snapshot",&native) &&
                    !calldata_bool(&native,"failed"),"lifecycle actual native owner failed");
                nr::lifecycle::RawKeyDiagnostic key;
                key.instance=instance;key.tick=token;key.selected=now;key.contentEpoch=epoch;
                key.rawPresent=static_cast<bool>(raw);key.currentPresent=static_cast<bool>(current);key.contextPresent=static_cast<bool>(context);
                if(raw)key.raw={raw->input.key.instanceId,raw->input.key.contentEpoch,raw->input.key.ingressSequence};
                if(current)key.current={current->input.key.instanceId,current->input.key.contentEpoch,current->input.key.ingressSequence};
                baselineRawOwner=reinterpret_cast<uintptr_t>(raw.get());baselineCurrentOwner=reinterpret_cast<uintptr_t>(current.get());
                baselineContextOwner=reinterpret_cast<uintptr_t>(context.get());
                std::cout<<"NR_LIFECYCLE_KEY {\"step\":"<<s.step<<",\"frame\":"<<s.frames<<",\"view\":"<<view
                    <<",\"instance\":"<<instance<<",\"tick\":"<<token<<",\"selected\":"<<now<<",\"ingress\":"<<ingress
                    <<",\"content_epoch\":"<<epoch<<",\"kind\":"<<kind<<",\"failure_mask\":"<<nr::lifecycle::raw_key_failure(key)
                    <<",\"raw_present\":"<<(raw?"true":"false")<<",\"current_present\":"<<(current?"true":"false")
                    <<",\"context_present\":"<<(context?"true":"false")<<",\"raw_owner\":"<<baselineRawOwner
                    <<",\"current_owner\":"<<baselineCurrentOwner<<",\"context_owner\":"<<baselineContextOwner
                    <<",\"placeholder\":"<<(placeholder?"true":"false")<<",\"lifecycle_reason\":"<<calldata_int(&data,"lifecycle_reason")
                    <<",\"resource_generation\":"<<calldata_int(&data,"resource_generation")
                    <<",\"history_epoch\":"<<calldata_int(&data,"history_epoch")<<",\"retiring\":"<<calldata_int(&native,"retiring")
                    <<",\"retired_generations\":"<<calldata_int(&native,"retired_generations")
                    <<",\"closed\":"<<(calldata_bool(&native,"closed")?"true":"false")
                    <<",\"physical_signal_known\":"<<(calldata_bool(&data,"physical_signal_known")?"true":"false")
                    <<",\"source_validity\":"<<calldata_int(&data,"source_validity")
                    <<",\"source_token\":"<<calldata_int(&data,"source_token")<<",\"device_token\":"<<calldata_int(&data,"device_token")
                    <<",\"requested_settings\":"<<calldata_int(&data,"settings_revision")
                    <<",\"applied_settings\":"<<calldata_int(&data,"applied_settings_revision")<<",\"raw_key\":";
                lifecycle_frame_key(raw?&raw->input.key:nullptr);std::cout<<",\"current_key\":";
                lifecycle_frame_key(current?&current->input.key:nullptr);
                std::cout<<",\"raw_tick\":"<<(raw?raw->input.canonicalTick:0)
                    <<",\"current_tick\":"<<(current?current->input.canonicalTick:0)<<",\"selected_job\":";
                if(job) {
                    std::cout<<"{\"frame\":";lifecycle_frame_key(&job->contract.key.frame);
                    std::cout<<",\"history_epoch\":"<<job->contract.key.historyEpoch
                        <<",\"resource_generation\":"<<job->contract.key.resourceGeneration
                        <<",\"submission_serial\":"<<job->contract.key.submissionSerial<<'}';
                } else std::cout<<"null";
                std::cout<<",\"capacity\":[";
                for(size_t slot=0;bridge && slot<3;++slot) {
                    if(slot)std::cout<<',';const auto receipt=bridge->ownership(slot);
                    std::cout<<"{\"slot\":"<<slot<<",\"has_key\":"<<(receipt.key?"true":"false")
                        <<",\"phase\":"<<static_cast<uint32_t>(receipt.phase)<<",\"reusable\":"<<(nr::interop::reusable(receipt)?"true":"false")
                        <<",\"consumer_open\":"<<(receipt.consumerOpen?"true":"false")<<",\"failed\":"<<(receipt.failed?"true":"false")<<'}';
                }
                std::cout<<"]}"<<std::endl;
                const nr::lifecycle::HostObservation observation{key,observedKind,
                    static_cast<nr::SourceValidity>(calldata_int(&data,"source_validity")),
                    static_cast<nr::Reason>(calldata_int(&data,"lifecycle_reason")),ingress,placeholder,
                    static_cast<bool>(bridge),static_cast<bool>(job),calldata_bool(&data,"physical_signal_known")};
                const auto path=nr::lifecycle::host_observation_path(observation);
                require(path!=nr::lifecycle::ObservationPath::Reject,"lifecycle immutable raw key provenance");
                const bool coldCurrent=path==nr::lifecycle::ObservationPath::ColdCurrentSource;
                if(placeholder || coldCurrent) {
                    require(!now && viewId==fixture->content,"lifecycle placeholder resurrected prior source pixels");
                    if(coldCurrent)std::cout<<"NR_LIFECYCLE_COLD {\"step\":"<<s.step<<",\"frame\":"<<s.frames
                        <<",\"instance\":"<<instance<<",\"tick\":"<<token<<",\"selected\":0,\"content_epoch\":"<<epoch
                        <<",\"source_content_id\":"<<viewId<<",\"accepted\":false,\"genuine_nr\":false,\"ordinary_ofa\":false}"<<std::endl;
                } else {
                    require(raw && current && context && raw->input.key.ingressSequence==now &&
                        raw->input.key.contentEpoch==epoch,"lifecycle immutable raw key provenance");
                    const auto renderer=nr::interop::renderer_luid(static_cast<ID3D11Device*>(gs_get_device_obj()));
                    require(nr::interop::same_luid(renderer,context->device()->GetAdapterLuid()),"lifecycle same LUID");
                    const auto currentBytes=nr_bootstrap_read_texture(current->original11.Get(),hdr,fixture->extent,1080);
                    currentHash=nr::qa::bootstrap::hash(currentBytes);
                    require(lifecycle_image_id(currentBytes,hdr,fixture->extent)==fixture->content,
                        "lifecycle current raw image is stale");
                    const auto rawBytes=nr_bootstrap_read_texture(raw->original11.Get(),hdr,fixture->extent,1080);
                    rawHash=nr::qa::bootstrap::hash(rawBytes);rawId=lifecycle_image_id(rawBytes,hdr,fixture->extent);
                    auto* output=static_cast<gs_texture_t*>(calldata_ptr(&native,"output_texture"));
                    require(output!=nullptr,"lifecycle selected output missing");
                    const auto outputBytes=nr_bootstrap_read_texture(
                        static_cast<ID3D11Texture2D*>(gs_texture_get_obj(output)),hdr,fixture->extent,1080);
                    outputHash=nr::qa::bootstrap::hash(outputBytes);
                    require(lifecycle_image_id(outputBytes,hdr,fixture->extent)==rawId && viewId==rawId,
                        "lifecycle output image is not the selected source phase");
                    if(kind==1)require(now==ingress && rawHash==currentHash,"lifecycle priming image phase");
                    else {
                        require(now+1==ingress,"lifecycle delayed image phase");
                        const auto old=std::find_if(s.images.begin(),s.images.end(),[&](const auto& image) {
                            return image.ingress==now;
                        });
                        require(old!=s.images.end() && old->hash==rawHash && old->content==rawId,
                            "lifecycle selected image differs from independent previous capture");
                    }
                    if(!s.native || kind!=3)require(outputHash==rawHash,"lifecycle untouched raw image changed");
                    if(kind==3) {
                        require(job && job->ready.load(std::memory_order_acquire) && job->published.load() &&
                            job->contract.key.frame==raw->input.key,"lifecycle exact published job provenance");
                        genuine=s.native && job->evaluated && job->composed && job->submitted &&
                            job->eligible && job->runtimeModule && job->flow.has_value() && !job->invalid;
                        ordinary=genuine && !job->flow->reset && job->guideReferenceKey.has_value();
                        selectedSerial=job->contract.key.submissionSerial;selectedHistory=job->contract.key.historyEpoch;
                        selectedSettingsRevision=job->contract.input.settings.revision.value;
                        if(s.native) {
                            const auto complete=job->completion();
                            const auto completed=complete.fence?complete.fence->GetCompletedValue():0;
                            require(complete.fence && complete.value && completed!=UINT64_MAX && completed>=complete.value,
                                "lifecycle exact physical model completion");
                            std::optional<nr::interop::Ownership> ownership;
                            for(size_t slot=0;bridge && slot<3;++slot) {
                                const auto observed=bridge->ownership(slot);
                                if(observed.key && *observed.key==job->contract.key)ownership.emplace(observed);
                            }
                            require(ownership && ownership->producer && ownership->worker &&
                                ownership->producerDone>=ownership->producer && ownership->workerDone>=ownership->worker,
                                "lifecycle exact producer worker ownership");
                            std::cout<<"NR_LIFECYCLE_WITNESS {\"step\":"<<s.step<<",\"frame\":"<<s.frames
                                <<",\"instance\":"<<instance<<",\"ingress\":"<<now
                                <<",\"content_epoch\":"<<job->contract.key.frame.contentEpoch
                                <<",\"history_epoch\":"<<job->contract.key.historyEpoch
                                <<",\"resource_generation\":"<<job->contract.key.resourceGeneration
                                <<",\"submission_serial\":"<<job->contract.key.submissionSerial
                                <<",\"settings_revision\":"<<job->contract.input.settings.revision.value
                                <<",\"creation_epoch\":"<<job->creationEpoch<<",\"submission_epoch\":"<<job->epoch
                                <<",\"evaluated\":"<<(job->evaluated?"true":"false")
                                <<",\"composed\":"<<(job->composed?"true":"false")
                                <<",\"submitted\":"<<(job->submitted?"true":"false")
                                <<",\"eligible\":"<<(job->eligible?"true":"false")
                                <<",\"reference_valid\":"<<(job->referenceValid?"true":"false")
                                <<",\"flow_reset\":"<<(job->flow && job->flow->reset?"true":"false")
                                <<",\"ordinary_ofa\":"<<(ordinary?"true":"false")
                                <<",\"code\":"<<job->code<<",\"invalid\":"<<job->invalid
                                <<",\"module_base\":"<<job->runtimeModule<<",\"module_path\":\""
                                <<std::filesystem::path(job->modulePath).generic_string()
                                <<"\",\"completion_value\":"<<complete.value<<",\"completion_done\":"<<completed
                                <<",\"producer\":"<<ownership->producer<<",\"producer_done\":"<<ownership->producerDone
                                <<",\"worker\":"<<ownership->worker<<",\"worker_done\":"<<ownership->workerDone
                                <<",\"consumer\":"<<ownership->consumer<<",\"consumer_done\":"<<ownership->consumerDone
                                <<",\"retired_generations\":"<<calldata_int(&native,"retired_generations")
                                <<",\"renderer_luid_high\":"<<renderer.HighPart<<",\"renderer_luid_low\":"<<renderer.LowPart
                                <<",\"worker_luid_high\":"<<context->device()->GetAdapterLuid().HighPart
                                <<",\"worker_luid_low\":"<<context->device()->GetAdapterLuid().LowPart
                                <<",\"raw_hash\":\""<<rawHash<<"\",\"output_hash\":\""<<outputHash<<"\"}\n"<<std::flush;
                            cold=genuine && job->flow->reset && !job->referenceValid;
                            if(job->evaluated)++s.evaluations;
                            if(cold)++s.resets;
                            if(cold) {
                                require(!job->guideReferenceKey && rawHash==outputHash,
                                    "lifecycle reset source restore or guide reference mismatch");
                            } else if(s.native)require(genuine && outputHash!=rawHash,"lifecycle actual NR/OFA no-op");
                        }
                    }
                    s.images[ingress%2]={ingress,currentHash,fixture->content};
                }
                calldata_free(&native);job.reset();raw.reset();current.reset();context.reset();bridge.reset();
                const bool settingsApplied=s.step!=10 || nr::lifecycle::settings_applied(s.requiredSettingsRevision,
                    static_cast<uint64_t>(calldata_int(&data,"applied_settings_revision")),
                    selectedSettingsRevision);
                if(!settingsApplied){genuine=false;ordinary=false;}
                const bool disabledObserved=s.step!=11 ||
                    (instance==s.disabledInstance && epoch>s.disabledContent &&
                     static_cast<uint64_t>(calldata_int(&data,"history_epoch"))>s.disabledHistory &&
                     static_cast<uint64_t>(calldata_int(&data,"resource_generation"))==s.disabledGeneration &&
                     placeholder && observedKind==nr::OutputKind::CurrentSource && now==0 &&
                     static_cast<nr::Reason>(calldata_int(&data,"lifecycle_reason"))==nr::Reason::Cancelled &&
                     viewId==fixture->content);
                accepted=nr::lifecycle::observation_accepted(path,s.native,s.step==3 || s.step==5 || s.step==11,
                    placeholder,genuine,cold,settingsApplied && disabledObserved);
                std::cout<<"NR_LIFECYCLE_SAMPLE {\"step\":"<<s.step<<",\"frame\":"<<s.frames
                    <<",\"genuine_nr\":"<<(genuine?"true":"false")<<",\"ordinary_ofa\":"<<(ordinary?"true":"false")
                    <<",\"image_hash\":\""<<viewHash<<"\"}\n";
                if(accepted)std::cout<<"NR_LIFECYCLE_ROW {\"step\":"<<s.step<<",\"frame\":"<<s.frames<<",\"case\":\""<<cases[s.step]
                    <<"\",\"instance\":"<<instance<<",\"tick\":"<<token<<",\"selected\":"<<now
                    <<",\"kind\":"<<calldata_int(&data,"kind")<<",\"ingress\":"<<calldata_int(&data,"ingress")
                    <<",\"content_epoch\":"<<epoch<<",\"history_epoch\":"<<calldata_int(&data,"history_epoch")
                    <<",\"resource_generation\":"<<calldata_int(&data,"resource_generation")
                    <<",\"submission_serial\":"<<selectedSerial<<",\"selected_history_epoch\":"<<selectedHistory
                    <<",\"settings_revision\":"<<calldata_int(&data,"settings_revision")
                    <<",\"applied_settings_revision\":"<<calldata_int(&data,"applied_settings_revision")
                    <<",\"source_token\":"<<calldata_int(&data,"source_token")
                    <<",\"device_token\":"<<calldata_int(&data,"device_token")
                    <<",\"placeholder\":"<<(placeholder?"true":"false")
                    <<",\"physical_signal_known\":"<<(calldata_bool(&data,"physical_signal_known")?"true":"false")
                    <<",\"validity_metadata_present\":"<<(calldata_bool(&data,"validity_metadata_present")?"true":"false")
                    <<",\"source_validity\":"<<calldata_int(&data,"source_validity")
                    <<",\"raw_hash\":\""<<rawHash<<"\",\"current_raw_hash\":\""<<currentHash
                    <<"\",\"output_hash\":\""<<outputHash<<"\",\"raw_content_id\":"<<rawId
                    <<",\"current_content_id\":"<<fixture->content<<",\"view_content_id\":"<<viewId
                    <<",\"image_hash\":\""<<viewHash<<"\",\"same_latch_views\":true"
                    <<",\"deterministic\":"<<(!s.native?"true":"false")
                    <<",\"genuine_nr\":"<<(genuine?"true":"false")
                    <<",\"ordinary_ofa\":"<<(ordinary?"true":"false")<<"}\n"<<std::flush;
            }
            if(s.step==3 || s.step==5)require(calldata_bool(&data,"placeholder") && now==0,"invalid source retained old selection");
            if(s.step==1 && epoch)require(epoch==s.lastEpoch,"identical valid pixels inferred source loss");
            if(s.step==14)require(s.replacement.accepts(instance),"new filter inherited instance identity");
            calldata_free(&data);
        }
        s.lastInstance=instance;if(epoch)s.lastEpoch=epoch;
        if(genuine && !cold)++s.applied;
        if(ordinary)++s.ordinary;
        if(accepted) {
            s.frames=0;
            if(++s.step==15)SetEvent(s.scope.complete);
        }else require(nr::lifecycle::advance_unaccepted_frame(s.frames),"lifecycle actual NR/OFA transition never applied in finite phase");
    }catch(const std::exception& e) {
        std::cout<<"NR_LIFECYCLE_FAILURE "<<e.what()<<std::endl;
        s.failed=true;SetEvent(s.scope.complete);
    }
}
bool run_lifecycle(const std::filesystem::path& root,bool& retained,bool native=false)
{
    Lifecycle s(root,native);
    s.scope.complete=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    require(s.scope.complete!=nullptr,"lifecycle pre-trigger completion subscription");
    obs_source_info info{};
    info.id="nr_task13_fixture";info.type=OBS_SOURCE_TYPE_INPUT;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
    info.get_name=fixture_name;info.create=fixture_create;info.destroy=fixture_destroy;
    info.get_width=fixture_extent;info.get_height=fixture_height;
    info.video_render=fixture_render;info.video_get_color_space=fixture_space;obs_register_source(&info);
    auto* settings=obs_data_create();obs_data_set_bool(settings,"nr_lifecycle_fixture",true);
    s.scope.source=obs_source_create_private(info.id,"Task13 explicit-validity source",settings);obs_data_release(settings);
    s.scope.filter=lifecycle_filter(root,native);
    require(s.scope.source && s.scope.filter,"lifecycle source/filter creation");
    obs_source_filter_add(s.scope.source,s.scope.filter);
    s.scope.scene=obs_scene_create_private("Task13 lifecycle scene");
    require(s.scope.scene && obs_scene_add(s.scope.scene,s.scope.source),"lifecycle scene");
    for(auto& view:s.scope.views) {
        view=obs_view_create();require(view!=nullptr,"lifecycle view");
        // Task12's preview/projector seam renders the actual filtered source
        // in its declared color space; the 64x64 foundation SDR canvas is not
        // a full-size HDR conversion surface.
        obs_view_set_source(view,0,s.scope.source);
    }
    obs_add_main_render_callback(lifecycle_draw,&s);
    const auto result=WaitForSingleObject(s.scope.complete,native?300000:60000);
    obs_enter_graphics();
    // Reuse the already-registered event for the exact graphics close state.
    // Worker exit alone is not final collection or callback/resource closure.
    ResetEvent(s.scope.complete);
    s.stopping=true;
    calldata_t data;calldata_init(&data);calldata_set_ptr(&data,"retain_exit_signal",&s.exited);
    require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_snapshot",&data),
        "lifecycle pre-stop worker subscription");calldata_free(&data);
    calldata_init(&data);
    require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_stop",&data),"lifecycle stop");
    const bool immediate=calldata_bool(&data,"closed");calldata_free(&data);obs_leave_graphics();
    const bool graphicsClosed=immediate || WaitForSingleObject(s.scope.complete,15000)==WAIT_OBJECT_0;
    const bool exited=immediate || (s.exited && s.exited->wait_for(15000));
    obs_enter_graphics();calldata_init(&data);
    proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_stop",&data);
    const bool closed=calldata_bool(&data,"closed");calldata_free(&data);obs_leave_graphics();
    obs_remove_main_render_callback(lifecycle_draw,&s);
    const bool allClosed=lifecycle_closure("post-close-before-source-destruction");
    retained=!closed || !allClosed;
    if(!retained)s.exited.reset(); // Exit observed and final Close confirmed: drop the worker-exit lease before anything unloads.
    if(retained) {
        // Unresolved production resources retain their own state/module. The host
        // keeper below prevents shutdown/unload; no success is fabricated.
        std::cout<<"NR_LIFECYCLE_RETAINED ownership unresolved"<<std::endl;
    }
    std::cout<<"NR_LIFECYCLE_CLEANUP {\"closed\":"<<(closed?"true":"false")
        <<",\"worker_exit\":"<<(exited?"true":"false")<<",\"retained\":"<<(retained?"true":"false")
        <<",\"genuine_nr_count\":"<<s.applied<<",\"ordinary_ofa_count\":"<<s.ordinary
        <<",\"actual_evaluations\":"<<s.evaluations<<",\"actual_reset_samples\":"<<s.resets<<"}\n"<<std::flush;
    return result==WAIT_OBJECT_0 && !s.failed && s.step==15 && graphicsClosed && closed && exited &&
        (!native || (s.applied>=12 && s.ordinary>0));
}
struct WarmupReturns:nr::model::ObservationSink {
    const std::array<std::shared_ptr<nr::model::QaSignal>,3> signals;
    std::optional<nr::JobKey> key;
    std::shared_ptr<nr::model::QaSignal> returned;
    nr::model::Fault fault=nr::model::Fault::None;
    explicit WarmupReturns(std::array<std::shared_ptr<nr::model::QaSignal>,3> value):signals(std::move(value)) {
        for(const auto& signal:signals)require(signal && signal->owner_module(),"original module warmup return signal");
    }
    nr::model::Observation* reserve(const nr::IngressSample& input,const nr::pipeline::TickPlan& plan) noexcept override {
        key.reset();returned.reset();
        if(plan.admission && plan.admission->slot<signals.size() && plan.admission->job.input==input) {
            key.emplace(plan.admission->job.key);
            returned=signals[plan.admission->slot];
            returned->bind(plan.admission->returned); // Subscribe before finish enqueues this exact job.
        }
        return nullptr; // No model-image observation or alternate processing path.
    }
    void capture(nr::model::Observation*,ID3D11Texture2D*,ID3D11Texture2D*,ID3D11Texture2D*) noexcept override {}
    nr::model::Fault qa_fault(const nr::JobKey& expected) const noexcept override {
        return key && *key==expected?fault:nr::model::Fault::None;
    }
};
struct Instances {
    std::array<Sampler,2> scopes;
    std::array<Fixture*,2> fixtures{};
    std::array<std::shared_ptr<nr::qa::InstanceObservations>,2> observations;
    std::array<std::shared_ptr<WarmupReturns>,2> warmupReturns;
    std::array<std::shared_ptr<nr::model::Job>,2> jobs;
    std::array<std::shared_ptr<nr::model::QaSignal>,2> exited;
    std::array<nr::lifecycle::CloseSnapshot,2> closure{};
    HANDLE trigger=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE rendered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE closed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    unsigned phase=0;
    bool measured=false,failed=false,stopping=false;
    bool callbackAdded=false;
    uint64_t renderedTick=0;
    std::array<bool,2> sourceClosed{};
    std::array<bool,2> nativeOriginal{};
    std::array<uint64_t,2> instance{},lastSerial{},lastTick{};
    std::array<unsigned,2> evaluations{};
    std::array<std::optional<nr::JobKey>,2> prior{};
    ~Instances() { CloseHandle(trigger);CloseHandle(rendered);CloseHandle(closed); }
};
Instances* retainedInstances=nullptr;
bool capacity_retained();
bool capacity_closed();
void capacity_request_stop();
void capacity_release();
bool instances_retained() { return retainedInstances!=nullptr || capacity_retained(); }
bool instances_closed() {
    if(!retainedInstances)return capacity_closed();
    for(size_t lane=0;lane<2;++lane) {
        const auto& s=*retainedInstances;
        if(!s.scopes[lane].filter)continue; // No filter or worker was created.
        if(!s.sourceClosed[lane])return false;
        if(s.nativeOriginal[lane] && (!nr::lifecycle::close_complete(s.closure[lane]) ||
            !s.exited[lane] || !s.exited[lane]->wait_for(0)))return false;
    }
    return true;
}
void instances_draw(void* opaque,uint32_t,uint32_t)
{
    auto& s=*static_cast<Instances*>(opaque);
    try {
        if(s.stopping) {
            for(size_t lane=0;lane<2;++lane)if(!s.sourceClosed[lane]) {
                if(!s.scopes[lane].filter)continue;
                calldata_t data;calldata_init(&data);
                require(proc_handler_call(obs_source_get_proc_handler(s.scopes[lane].filter),"nr_native_stop",&data),
                    "instance exact graphics close");
                const bool closed=calldata_bool(&data,"closed");calldata_free(&data);
                if(closed) {
                    if(s.nativeOriginal[lane]) {
                        calldata_init(&data);calldata_set_ptr(&data,"snapshot",&s.closure[lane]);
                        require(proc_handler_call(obs_get_proc_handler(),"nr_retirement_snapshot",&data),
                            "instance surviving primitive closure");
                        calldata_free(&data);
                        require(s.closure[lane].instance==s.instance[lane] &&
                            nr::lifecycle::close_complete(s.closure[lane]),"foreign/incomplete instance closure");
                    }
                    s.sourceClosed[lane]=true;
                }
            }
            if(instances_closed())SetEvent(s.closed);
            return;
        }
        if(WaitForSingleObject(s.trigger,0)!=WAIT_OBJECT_0)return;
        const auto canonical=nr::sampler::canonical_tick(obs_get_video_frame_time(),obs_get_frame_interval_ns());
        if(canonical<=s.renderedTick)return; // The OBS render event, not host speed, advances input.
        ResetEvent(s.trigger);
        s.renderedTick=canonical;
        std::array<uint64_t,2> hashes{},selected{},tick{},ingress{},serial{};
        std::array<uint32_t,2> ids{};
        const std::array<unsigned,2> beforeRenders{s.fixtures[0]->renders,s.fixtures[1]->renders};
        const auto first=s.phase%2;
        for(unsigned view=0;view<3;++view)for(unsigned order=0;order<2;++order) {
            const auto lane=(first+order)%2;
            auto& scope=s.scopes[lane];
            const auto bytes=lifecycle_view(scope.views[view],false,1920);
            const auto hash=nr::qa::bootstrap::hash(bytes);
            const auto id=lifecycle_image_id(bytes,false,1920);
            calldata_t sampler;calldata_init(&sampler);
            require(proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_sampler_snapshot",&sampler),
                "instance production sampler snapshot");
            const auto instance=static_cast<uint64_t>(calldata_int(&sampler,"instance"));
            const auto token=static_cast<uint64_t>(calldata_int(&sampler,"tick"));
            const auto now=static_cast<uint64_t>(calldata_int(&sampler,"selected"));
            const auto input=static_cast<uint64_t>(calldata_int(&sampler,"ingress"));
            const auto kind=static_cast<nr::OutputKind>(calldata_int(&sampler,"kind"));
            std::shared_ptr<const nr::obs_adapter::Capture> raw,current;
            std::shared_ptr<nr::model::Job> currentJob,selectedJob;
            std::shared_ptr<nr::interop::Bridge> bridge;
            calldata_t native;calldata_init(&native);
            calldata_set_ptr(&native,"retain_job",&currentJob);calldata_set_ptr(&native,"retain_selected_job",&selectedJob);
            calldata_set_ptr(&native,"retain_raw",&raw);calldata_set_ptr(&native,"retain_current_raw",&current);
            calldata_set_ptr(&native,"retain_bridge",&bridge);
            const bool snapshot=proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_native_snapshot",&native);
            const bool binding=instance && raw && current && bridge && currentJob &&
                currentJob->contract.input==current->input && raw->input.key.instanceId==instance &&
                current->input.key.instanceId==instance && current->input.key.ingressSequence==input &&
                raw->input.key.ingressSequence==now;
            const auto* publicState=calldata_string(&native,"public_state");
            const auto* publicReason=calldata_string(&native,"public_reason");
            std::ostringstream bindingRecord;
            bindingRecord<<"NR_INSTANCE_BINDING {\"measured\":"<<(s.measured?"true":"false")
                <<",\"phase\":"<<s.phase<<",\"lane\":"<<lane<<",\"view\":"<<view
                <<",\"instance\":"<<instance<<",\"tick\":"<<token<<",\"clock\":"<<calldata_int(&sampler,"clock")
                <<",\"ingress\":"<<input<<",\"selected\":"<<now<<",\"kind\":"<<static_cast<unsigned>(kind)
                <<",\"snapshot\":"<<(snapshot?"true":"false")
                <<",\"failed\":"<<(calldata_bool(&native,"failed")?"true":"false")
                <<",\"placeholder\":"<<(calldata_bool(&native,"placeholder")?"true":"false")
                <<",\"public_state\":";
            nr::qa::write_instance_json_string(bindingRecord,publicState);
            bindingRecord<<",\"public_reason\":";
            nr::qa::write_instance_json_string(bindingRecord,publicReason);
            bindingRecord<<",\"raw_present\":"<<(raw?"true":"false")<<",\"current_present\":"<<(current?"true":"false")
                <<",\"bridge_present\":"<<(bridge?"true":"false")<<",\"job_present\":"<<(currentJob?"true":"false")
                <<",\"job_matches_current\":"<<(currentJob && current && currentJob->contract.input==current->input?"true":"false")
                <<",\"raw_instance\":"<<(raw?raw->input.key.instanceId:0)
                <<",\"current_instance\":"<<(current?current->input.key.instanceId:0)
                <<",\"raw_ingress\":"<<(raw?raw->input.key.ingressSequence:0)
                <<",\"current_ingress\":"<<(current?current->input.key.ingressSequence:0)
                <<",\"current_tick\":"<<(current?current->input.canonicalTick:0)
                <<",\"preceding_tick\":"<<(current?current->input.precedingCanonicalTick:0)
                <<",\"job_tick\":"<<(currentJob?currentJob->contract.input.canonicalTick:0)
                <<",\"job_ingress\":"<<(currentJob?currentJob->contract.input.key.ingressSequence:0)
                <<",\"binding\":"<<(binding?"true":"false")<<"}\n";
            std::osyncstream(std::cout)<<bindingRecord.str()<<std::flush;
            require(snapshot && !calldata_bool(&native,"failed"),"instance actual native owner failed");
            const bool warmupCurrent=!s.measured && kind==nr::OutputKind::CurrentSource;
            if(warmupCurrent)require(instance && !now && !selectedJob &&
                ((!raw && !current) || binding),"warmup current-source owner provenance");
            else require(binding,"instance immutable owner binding");
            if(!view) {
                hashes[lane]=hash;selected[lane]=now;tick[lane]=token;ingress[lane]=input;ids[lane]=id;
                serial[lane]=currentJob?currentJob->contract.key.submissionSerial:0;
                s.instance[lane]=instance;
                // A saved job after capture-free bypass is not this action's dispatch.
                s.jobs[lane]=binding && current->input.canonicalTick==token?currentJob:nullptr;
            } else require(hashes[lane]==hash && selected[lane]==now && tick[lane]==token &&
                ingress[lane]==input && ids[lane]==id && serial[lane]==(currentJob?currentJob->contract.key.submissionSerial:0),
                "actual repeated OBS rendering changed capture/job/key/pixels");
            for(size_t slot=0;bridge && slot<3;++slot) {
                const auto owner=bridge->ownership(slot);
                require(!owner.key || owner.key->frame.instanceId==instance,"foreign Bridge slot ownership");
            }
            require(calldata_int(&native,"retired_generations")<=2,"instance retired generation bound");
            if(s.measured) {
                require(s.fixtures[lane]->renders==beforeRenders[lane]+1,"repeated OBS view recaptured original source");
                const auto expectedCurrent=static_cast<uint32_t>((lane?9:1)+s.phase);
                const auto currentBytes=nr_bootstrap_read_texture(current->original11.Get(),false,1920,1080);
                const auto rawBytes=nr_bootstrap_read_texture(raw->original11.Get(),false,1920,1080);
                const auto currentHash=nr::qa::bootstrap::hash(currentBytes),rawHash=nr::qa::bootstrap::hash(rawBytes);
                const auto rawId=lifecycle_image_id(rawBytes,false,1920);
                const auto currentId=lifecycle_image_id(currentBytes,false,1920);
                require(currentId==expectedCurrent && id==rawId &&
                    (lane?id>=9 && id<=14:id>=1 && id<=6),"actual output has foreign analytic content ID");
                if(s.phase)require(rawId==expectedCurrent-1 && now+1==input &&
                    raw->input.canonicalTick==current->input.precedingCanonicalTick,"exact canonical selected predecessor");
                if(lane==0 && s.phase==3) {
                    require(kind==nr::OutputKind::DelayedRaw && !selectedJob && rawHash==hash,
                        "late A resurrected NR publication");
                } else {
                    require(kind==nr::OutputKind::DelayedNr && selectedJob && selectedJob->evaluated &&
                        selectedJob->composed && selectedJob->submitted && selectedJob->published.load() &&
                        selectedJob->contract.key.frame==raw->input.key && s.prior[lane] &&
                        selectedJob->contract.key==*s.prior[lane],"exact instance-selected prior JobKey not published");
                }
                require(current->input.settings.strength.value==(lane?.75f:.25f),"foreign instance composition settings");
                std::ostringstream record;
                record<<"NR_INSTANCE_VIEW {\"lane\":"<<lane<<",\"step\":"<<s.phase<<",\"view\":"<<view
                    <<",\"instance\":"<<instance<<",\"tick\":"<<token<<",\"ingress\":"<<input<<",\"selected\":"<<now
                    <<",\"kind\":"<<static_cast<unsigned>(kind)<<",\"content_epoch\":"<<current->input.key.contentEpoch
                    <<",\"history_epoch\":"<<currentJob->contract.key.historyEpoch
                    <<",\"resource_generation\":"<<currentJob->contract.key.resourceGeneration
                    <<",\"content_id\":"<<id<<",\"current_content_id\":"<<currentId
                    <<",\"output_hash\":\""<<hash<<"\",\"raw_hash\":\""<<rawHash<<"\",\"current_hash\":\""<<currentHash
                    <<"\",\"settings_strength\":"<<current->input.settings.strength.value
                    <<",\"source_renders\":"<<s.fixtures[lane]->renders
                    <<",\"retired_generations\":"<<calldata_int(&native,"retired_generations")<<",\"selected_job\":";
                if(selectedJob) {
                    const auto& key=selectedJob->contract.key;
                    record<<"{\"instance\":"<<key.frame.instanceId<<",\"content_epoch\":"<<key.frame.contentEpoch
                        <<",\"ingress\":"<<key.frame.ingressSequence<<",\"history_epoch\":"<<key.historyEpoch
                        <<",\"resource_generation\":"<<key.resourceGeneration<<",\"submission_serial\":"<<key.submissionSerial<<'}';
                }else record<<"null";
                record<<"}\n";std::osyncstream(std::cout)<<record.str()<<std::flush;
            }
            calldata_free(&native);calldata_free(&sampler);
        }
        require(s.instance[0]!=s.instance[1] && tick[0]==tick[1] && hashes[0]!=hashes[1],
            "instance identity/canonical/content isolation");
        if(s.measured) {
            for(size_t lane=0;lane<2;++lane) {
                require(s.jobs[lane]->contract.key.submissionSerial==s.lastSerial[lane]+1 &&
                    tick[lane]==s.lastTick[lane]+1,"more than one measured dispatch or missed canonical predecessor");
                s.lastSerial[lane]=s.jobs[lane]->contract.key.submissionSerial;s.lastTick[lane]=tick[lane];
            }
            if(s.phase==3)s.observations[0]->release_publication(); // Both render cutoffs and all repeated views are latched.
        }
        SetEvent(s.rendered);
    }catch(const std::exception& error) {
        s.failed=true;
        std::osyncstream(std::cout)<<"NR_INSTANCE_FAILURE "<<error.what()<<'\n'<<std::flush;
        SetEvent(s.rendered);
    }
}
void instances_release()
{
    if(!retainedInstances){capacity_release();return;}
    obs_enter_graphics();
    auto* s=retainedInstances;
    if(instances_closed()) {
        if(s->callbackAdded)obs_remove_main_render_callback(instances_draw,s);
        retainedInstances=nullptr;
        delete s; // Includes every partially-created view, scene, source and filter.
    }
    obs_leave_graphics();
}
void instances_request_stop()
{
    if(!retainedInstances){capacity_request_stop();return;}
    auto& s=*retainedInstances;
    obs_enter_graphics();
    try {
        if(!s.stopping) {
            for(size_t lane=0;lane<2;++lane)if(s.scopes[lane].filter) {
                std::shared_ptr<nr::interop::Context> context;
                calldata_t data;calldata_init(&data);
                require(proc_handler_call(obs_source_get_proc_handler(s.scopes[lane].filter),"nr_sampler_snapshot",&data),
                    "pre-stop original instance identity");
                s.instance[lane]=static_cast<uint64_t>(calldata_int(&data,"instance"));calldata_free(&data);
                calldata_init(&data);calldata_set_ptr(&data,"retain_exit_signal",&s.exited[lane]);
                calldata_set_ptr(&data,"retain_context",&context);
                require(proc_handler_call(obs_source_get_proc_handler(s.scopes[lane].filter),"nr_native_snapshot",&data),
                    "pre-stop original instance worker subscription");
                calldata_free(&data);s.nativeOriginal[lane]=static_cast<bool>(context);
                require(!context || s.exited[lane],"original native worker exit signal unavailable");
            }
            s.jobs={};for(auto& prior:s.prior)prior.reset();
            s.stopping=true;
            if(!s.callbackAdded) { obs_add_main_render_callback(instances_draw,&s);s.callbackAdded=true; }
        }
    } catch(...) { obs_leave_graphics();throw; }
    obs_leave_graphics();
}
bool instances_stop(bool& retained)
{
    instances_request_stop();
    auto& s=*retainedInstances;
    if(WaitForSingleObject(s.closed,30000)!=WAIT_OBJECT_0)return false;
    obs_enter_graphics();
    const bool closed=instances_closed();
    if(closed)for(size_t lane=0;lane<2;++lane)if(s.scopes[lane].filter) {
        if(s.nativeOriginal[lane])lifecycle_write_closure(s.closure[lane],"task14-instance-closed",lane);
        std::osyncstream(std::cout)<<"NR_INSTANCE_CLEANUP {\"lane\":"<<lane<<",\"instance\":"<<s.instance[lane]
            <<",\"closed\":true,\"worker_created\":"<<(s.exited[lane]?"true":"false")
            <<",\"worker_exit\":"<<(s.exited[lane] && s.exited[lane]->wait_for(0)?"true":"false")
            <<",\"retained\":false,\"actual_evaluations\":"<<s.evaluations[lane]<<"}\n"<<std::flush;
    }
    obs_leave_graphics();
    if(closed) { instances_release();retained=false; }
    return closed;
}
bool run_instances(const std::filesystem::path& root,bool& retained)
{
    auto* s=new Instances;
    retainedInstances=s;retained=true; // Originals survive every uncertain return.
    bool graphicsEntered=false;
    try {
    require(s->trigger && s->rendered && s->closed,"pre-trigger instance graphics subscriptions");
    obs_source_info info{};
    info.id="nr_task14_fixture";info.type=OBS_SOURCE_TYPE_INPUT;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
    info.get_name=fixture_name;info.create=fixture_create;info.destroy=fixture_destroy;
    info.get_width=fixture_extent;info.get_height=fixture_height;
    info.video_render=fixture_render;info.video_get_color_space=fixture_space;obs_register_source(&info);
    for(size_t lane=0;lane<2;++lane) {
        auto& scope=s->scopes[lane];
        auto* sourceSettings=obs_data_create();obs_data_set_bool(sourceSettings,"nr_lifecycle_fixture",true);
        scope.source=obs_source_create_private(info.id,lane?"Task14 content B":"Task14 content A",sourceSettings);
        obs_data_release(sourceSettings);s->fixtures[lane]=fixture;
        require(scope.source && fixture,"actual two source creation");
        fixture->content=lane?9:1;
        auto* filterSettings=obs_data_create();
        obs_data_set_string(filterSettings,"nr_shader_root",(root/"bin/64bit").string().c_str());
        obs_data_set_string(filterSettings,"nr_data_root",(root/"profile/ngx-data").string().c_str());
        obs_data_set_double(filterSettings,"nr_strength",lane?.75:.25);
        obs_data_set_bool(filterSettings,"nr_qa_observe",true);
        scope.filter=obs_source_create_private("susemi_nr_filter",lane?"Task14 filter B":"Task14 filter A",filterSettings);
        obs_data_release(filterSettings);
        require(scope.filter!=nullptr,"actual two filter creation");obs_source_filter_add(scope.source,scope.filter);
        std::array<std::shared_ptr<nr::model::QaSignal>,3> signals;
        calldata_t subscription;calldata_init(&subscription);calldata_set_ptr(&subscription,"retain_dispatch_signals",&signals);
        require(proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_native_snapshot",&subscription),
            "original pre-trigger warmup return subscriptions");calldata_free(&subscription);
        s->warmupReturns[lane]=std::make_shared<WarmupReturns>(std::move(signals));
        std::shared_ptr<nr::model::ObservationSink> sink=s->warmupReturns[lane];
        calldata_init(&subscription);calldata_set_ptr(&subscription,"observer",&sink);
        require(proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_native_observe",&subscription),
            "original warmup return observer");calldata_free(&subscription);
        scope.scene=obs_scene_create_private(lane?"Task14 program B":"Task14 program A");
        require(scope.scene && obs_scene_add(scope.scene,scope.source),"repeated source program reference");
        for(size_t view=0;view<3;++view) {
            scope.views[view]=obs_view_create();require(scope.views[view]!=nullptr,"actual preview/program/projector view");
            obs_view_set_source(scope.views[view],0,view==1?obs_scene_get_source(scope.scene):scope.source);
        }
    }
    obs_add_main_render_callback(instances_draw,s);
    s->callbackAdded=true;
    const auto trigger=[&] {
        ResetEvent(s->rendered);SetEvent(s->trigger);
        require(WaitForSingleObject(s->rendered,15000)==WAIT_OBJECT_0 && !s->failed,"instance render event/refusal");
    };
    const auto completed=[&](size_t lane,size_t step,bool measured) {
        const auto job=s->jobs[lane];require(job!=nullptr,"actual current dispatch owner");
        if(measured) {
            auto& observation=s->observations[lane]->at(step);
            require(observation.returned->wait_for(30000) &&
                WaitForSingleObject(observation.observed,30000)==WAIT_OBJECT_0 &&
                observation.finished.load(std::memory_order_acquire) && !observation.refused.load() &&
                observation.key==job->contract.key,"pre-subscribed exact dispatch/physical completion");
        } else {
            require(WaitForSingleObject(job->observed,30000)==WAIT_OBJECT_0,"warmup exact pre-submit job event");
            const auto& witness=*s->warmupReturns[lane];
            require(witness.key==job->contract.key && witness.returned && witness.returned->wait_for(30000),
                "same-key actual scheduler return after model readiness");
        }
        require(job->ready.load(std::memory_order_acquire),"actual worker readiness");
        if(measured) {
            const auto physical=job->completion();
            std::ostringstream completionRecord;
            completionRecord<<"NR_INSTANCE_COMPLETION {\"lane\":"<<lane<<",\"step\":"<<step
                <<",\"instance\":"<<job->contract.key.frame.instanceId
                <<",\"tick\":"<<job->contract.input.canonicalTick
                <<",\"serial\":"<<job->contract.key.submissionSerial
                <<",\"history_epoch\":"<<job->contract.key.historyEpoch<<",\"code\":"<<job->code
                <<",\"evaluated\":"<<(job->evaluated?"true":"false")
                <<",\"composed\":"<<(job->composed?"true":"false")
                <<",\"submitted\":"<<(job->submitted?"true":"false")
                <<",\"eligible\":"<<(job->eligible?"true":"false")<<",\"invalid\":"<<job->invalid
                <<",\"flow_present\":"<<(job->flow?"true":"false")
                <<",\"flow_reset\":"<<(job->flow && job->flow->reset?"true":"false")
                <<",\"fence_present\":"<<(physical.fence?"true":"false")<<",\"required_fence\":"<<physical.value
                <<",\"actual_fence\":"<<(physical.fence?physical.fence->GetCompletedValue():0)
                <<",\"diagnostic\":";
            nr::qa::write_instance_json_string(completionRecord,job->diagnostic.c_str());
            completionRecord<<"}\n";
            std::osyncstream(std::cout)<<completionRecord.str()<<std::flush;
            require(job->evaluated && job->composed && job->submitted && physical.fence &&
                nr::interop::completion(physical.value,physical.fence->GetCompletedValue())==nr::interop::Completion::Complete,
                "actual Feature18 evaluation/fence");
            ++s->evaluations[lane];
            const auto& key=job->contract.key;
            std::ostringstream record;
            record<<"NR_INSTANCE_DISPATCH {\"lane\":"<<lane<<",\"step\":"<<step<<",\"tick\":"<<job->contract.input.canonicalTick
                <<",\"instance\":"<<key.frame.instanceId<<",\"content_epoch\":"<<key.frame.contentEpoch
                <<",\"history_epoch\":"<<key.historyEpoch<<",\"resource_generation\":"<<key.resourceGeneration
                <<",\"submission_serial\":"<<key.submissionSerial<<",\"ingress\":"<<key.frame.ingressSequence
                <<",\"evaluated\":true,\"composed\":true,\"submitted\":true,\"returned\":true,\"physical_complete\":true}\n";
            std::osyncstream(std::cout)<<record.str()<<std::flush;
        }
    };
        // Exact render and job events bound preparation. Expensive initialization
        // may cause real gaps; those bypasses are observations, not fake dispatches.
        bool ordinary=false;
        for(unsigned phase=0;phase<64 && !ordinary;++phase) {
            s->phase=phase;trigger();
            ordinary=true;
            for(size_t lane=0;lane<2;++lane) {
                const auto job=s->jobs[lane];
                if(!job) { ordinary=false;continue; }
                completed(lane,phase,false);
                const auto physical=job->completion();
                const bool prepared=job->evaluated && job->composed && job->submitted &&
                    job->flow && !job->flow->reset && physical.fence && physical.value &&
                    nr::interop::completion(physical.value,physical.fence->GetCompletedValue())==nr::interop::Completion::Complete;
                ordinary=ordinary && prepared;
                std::osyncstream(std::cout)<<"NR_INSTANCE_PREPARATION {\"lane\":"<<lane
                    <<",\"phase\":"<<phase<<",\"tick\":"<<job->contract.input.canonicalTick
                    <<",\"serial\":"<<job->contract.key.submissionSerial<<",\"code\":"<<job->code
                    <<",\"ordinary_physical_complete\":"<<(prepared?"true":"false")<<"}\n"<<std::flush;
            }
        }
        require(ordinary,"bounded ordinary preparation of both instances not proven");
        obs_enter_graphics();graphicsEntered=true;
        for(size_t lane=0;lane<2;++lane) {
            std::shared_ptr<nr::interop::Context> context;
            std::array<std::shared_ptr<nr::model::QaSignal>,3> signals;
            calldata_t data;calldata_init(&data);calldata_set_ptr(&data,"retain_context",&context);
            calldata_set_ptr(&data,"retain_dispatch_signals",&signals);
            require(proc_handler_call(obs_source_get_proc_handler(s->scopes[lane].filter),"nr_native_snapshot",&data),
                "instance actual context");calldata_free(&data);
            require(context && s->jobs[lane]->flow && !s->jobs[lane]->flow->reset,"ordinary warmup did not prepare model");
            s->observations[lane]=std::make_shared<nr::qa::InstanceObservations>(context->device(),*s->jobs[lane],std::move(signals));
            std::shared_ptr<nr::model::ObservationSink> sink=s->observations[lane];
            calldata_init(&data);calldata_set_ptr(&data,"observer",&sink);
            require(proc_handler_call(obs_source_get_proc_handler(s->scopes[lane].filter),"nr_native_observe",&data),
                "pre-action instance observation binding");calldata_free(&data);
            s->prior[lane].emplace(s->jobs[lane]->contract.key);
            s->lastSerial[lane]=s->jobs[lane]->contract.key.submissionSerial;
            s->lastTick[lane]=s->jobs[lane]->contract.input.canonicalTick;
            const auto& job=*s->jobs[lane];
            std::shared_ptr<const nr::obs_adapter::Capture> current;
            calldata_init(&data);calldata_set_ptr(&data,"retain_current_raw",&current);
            require(proc_handler_call(obs_source_get_proc_handler(s->scopes[lane].filter),"nr_native_snapshot",&data),
                "ordinary baseline immutable capture");calldata_free(&data);
            require(current && current->input==job.contract.input,"ordinary baseline current job provenance");
            const auto pixels=nr_bootstrap_read_texture(current->original11.Get(),false,1920,1080);
            const auto content=lifecycle_image_id(pixels,false,1920);
            require(content==(lane?9u:1u),"ordinary baseline actual source content");
            const auto physical=job.completion();
            require(job.ready.load(std::memory_order_acquire) && job.evaluated && job.composed && job.submitted &&
                job.flow && !job.flow->reset && physical.fence && physical.value &&
                nr::interop::completion(physical.value,physical.fence->GetCompletedValue())==nr::interop::Completion::Complete,
                "ordinary baseline real model completion");
            const auto& key=job.contract.key;
            std::ostringstream baseline;
            baseline<<"NR_INSTANCE_BASELINE {\"lane\":"<<lane<<",\"instance\":"<<key.frame.instanceId
                <<",\"tick\":"<<job.contract.input.canonicalTick<<",\"content_epoch\":"<<key.frame.contentEpoch
                <<",\"history_epoch\":"<<key.historyEpoch<<",\"resource_generation\":"<<key.resourceGeneration
                <<",\"submission_serial\":"<<key.submissionSerial<<",\"ingress\":"<<key.frame.ingressSequence
                <<",\"source_renders\":"<<s->fixtures[lane]->renders<<",\"current_hash\":\""
                <<nr::qa::bootstrap::hash(pixels)<<"\",\"content_id\":"<<content
                <<",\"settings_strength\":"<<job.contract.input.settings.strength.value
                <<",\"evaluated\":true,\"composed\":true,\"submitted\":true,\"returned\":true,"
                  "\"physical_complete\":true,\"ordinary\":true}\n";
            std::osyncstream(std::cout)<<baseline.str()<<std::flush;
        }
        obs_leave_graphics();graphicsEntered=false;
        s->measured=true;
        for(unsigned step=0;step<6;++step) {
            obs_enter_graphics();graphicsEntered=true;s->phase=step;
            for(size_t lane=0;lane<2;++lane) {
                s->fixtures[lane]->content=static_cast<uint32_t>((lane?9:1)+step);
                s->observations[lane]->arm(step);
                if(lane==0 && step==2)s->observations[lane]->withhold_publication(step);
            }
            obs_leave_graphics();graphicsEntered=false;trigger();
            completed(0,step,true); // Actual A2 completion/return/guide settling is never delayed by QA publication.
            completed(1,step,true);
            for(size_t lane=0;lane<2;++lane)s->prior[lane].emplace(s->jobs[lane]->contract.key);
        }
        require(instances_stop(retained),"two actual instance closures and worker exits");
        std::osyncstream(std::cout)<<"NR_INSTANCE_SCOPE real_filters=2 repeated_OBS_views=true native=true cleanup=complete\n"<<std::flush;
        return true;
    }catch(const std::exception& error) {
        if(graphicsEntered)obs_leave_graphics();
        std::osyncstream(std::cout)<<"NR_INSTANCE_FAILURE "<<error.what()<<'\n'<<std::flush;
        try {
            if(retainedInstances && instances_stop(retained)) {
                std::osyncstream(std::cout)<<"NR_INSTANCE_FAILURE_CLEANUP normal=true retained=false\n"<<std::flush;
                return false;
            }
        } catch(const std::exception& cleanup) {
            std::osyncstream(std::cout)<<"NR_INSTANCE_CLEANUP_FAILURE "<<cleanup.what()<<'\n'<<std::flush;
        }
        std::osyncstream(std::cout)<<"NR_INSTANCE_RETAINED originals=true normal_stop_handoff=true\n"<<std::flush;
        // No forced completion of an uncertain late dependency.
        return false;
    }
}
struct Capacity {
    struct Held {
        std::shared_ptr<nr::interop::Bridge> bridge;
        nr::interop::Consumer consumer;
        Microsoft::WRL::ComPtr<ID3D11Fence> releaseFence;
        std::shared_ptr<nr::interop::ConsumerEvent> releaseEvent;
        std::shared_ptr<const nr::interop::ConsumerEnd> ending;
    };
    struct Receipt {
        nr::JobKey key;
        uint64_t tick;
        size_t envelope;
        unsigned phase;
        bool waiting;
    };
    std::array<Sampler,2> scopes;
    std::array<Fixture*,2> fixtures{};
    std::array<std::shared_ptr<nr::qa::CapacityObservations>,2> observations;
    std::array<std::array<std::optional<Receipt>,80>,2> receipts;
    std::array<size_t,2> receiptCount{},consumed{},armed{};
    std::array<std::shared_ptr<nr::model::Job>,2> latest;
    std::array<std::shared_ptr<nr::interop::Bridge>,2> bridges;
    std::vector<Held> slots,retired;
    std::array<std::shared_ptr<nr::model::QaSignal>,2> exited;
    std::array<nr::lifecycle::CloseSnapshot,2> closure{};
    std::array<nr::pipeline::Counters,2> beforeStop{};
    std::array<uint64_t,2> instance{};
    std::optional<nr::Epochs> independentB;
    std::array<bool,2> sourceClosed{};
    std::array<bool,2> nativeOriginal{};
    HANDLE trigger=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE rendered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE closed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE recovered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    unsigned phase=0,waitingFrames=0;
    bool failed=false,waiting=false,stopping=false,callbackAdded=false;
    ~Capacity() { CloseHandle(trigger);CloseHandle(rendered);CloseHandle(closed);CloseHandle(recovered); }
};
Capacity* retainedCapacity=nullptr;
bool capacity_retained() { return retainedCapacity!=nullptr; }
bool capacity_closed() {
    if(!retainedCapacity)return false;
    const auto& s=*retainedCapacity;
    if(!s.slots.empty() || !s.retired.empty())return false;
    for(size_t lane=0;lane<2;++lane)if(s.scopes[lane].filter) {
        if(!s.sourceClosed[lane])return false;
        if(s.nativeOriginal[lane] && (!nr::lifecycle::close_complete(s.closure[lane]) ||
            !s.exited[lane] || !s.exited[lane]->wait_for(0)))return false;
    }
    return true;
}
void capacity_key(std::ostream& out,const nr::JobKey* key) {
    if(!key){out<<"null";return;}
    out<<"{\"instance\":"<<key->frame.instanceId<<",\"content_epoch\":"<<key->frame.contentEpoch
        <<",\"ingress\":"<<key->frame.ingressSequence<<",\"history_epoch\":"<<key->historyEpoch
        <<",\"resource_generation\":"<<key->resourceGeneration<<",\"submission_serial\":"<<key->submissionSerial<<'}';
}
void capacity_hold(Capacity& s,std::vector<Capacity::Held>& owners,
    const std::shared_ptr<nr::model::Job>& job,const std::shared_ptr<nr::interop::Bridge>& bridge) {
    require(job && bridge && job->ready.load(std::memory_order_acquire),"actual completed aged capacity owner");
    for(size_t slot=0;slot<3;++slot) {
        const auto physical=bridge->ownership(slot);
        if(!physical.key || *physical.key!=job->contract.key)continue;
        const auto consumer=bridge->acquire_raw({job->contract.key,slot});
        require(consumer.has_value(),"actual aged capacity consumer");
        Microsoft::WRL::ComPtr<ID3D11Fence> originalFence;
        originalFence=bridge->consumer_fence();
        require(static_cast<bool>(originalFence),"original aged capacity consumer fence");
        owners.push_back({bridge,*consumer,originalFence,{},{}});
        require(owners.size()<=3 && consumer->ticket.key.frame.instanceId==s.instance[0],"bounded A-only native consumers");
        return;
    }
    throw std::runtime_error("actual aged capacity ticket missing");
}
void capacity_draw(void* opaque,uint32_t,uint32_t) {
    auto& s=*static_cast<Capacity*>(opaque);
    try {
        if(s.stopping) {
            // Only this callback owns Bridge's recorded graphics thread. Preserve
            // unresolved consumers/events; no wait or forced release on graphics.
            for(auto* owners:{&s.slots,&s.retired}) {
                bool completed=true;
                for(auto& owner:*owners) {
                    require(owner.bridge && owner.releaseFence,"stop original capacity consumer fence");
                    const auto* liveFence=owner.bridge->consumer_fence();
                    if(liveFence) {
                        require(liveFence==owner.releaseFence.Get(),"stop original capacity fence identity");
                        const auto physical=owner.bridge->ownership(owner.consumer.ticket.slot);
                        require(physical.key && *physical.key==owner.consumer.ticket.key &&
                            physical.consumerLease==owner.consumer.leaseValue && !physical.failed,
                            "stop original capacity consumer");
                        if(owner.releaseFence->GetCompletedValue()==UINT64_MAX){completed=false;continue;}
                        if(physical.consumerOpen) {
                            require(!owner.ending && owner.bridge->owns_consumer(owner.consumer),"stop exact open acquisition");
                            if(!owner.releaseEvent)owner.releaseEvent=nr::interop::ConsumerEvent::create();
                            require(static_cast<bool>(owner.releaseEvent),"stop fresh original consumer event");
                            owner.ending=owner.bridge->end_consumer(owner.consumer,owner.releaseEvent);
                            require(static_cast<bool>(owner.ending),"stop end original capacity consumer");
                        }
                        const auto closed=owner.bridge->ownership(owner.consumer.ticket.slot);
                        require(owner.ending && owner.ending->consumer.fence.Get()==owner.releaseFence.Get() &&
                            owner.ending->event==owner.releaseEvent && closed.key && *closed.key==owner.consumer.ticket.key &&
                            closed.consumerLease==owner.consumer.leaseValue &&
                            closed.consumer==owner.ending->completionValue && !closed.failed && !closed.consumerOpen,
                            "stop closed original capacity consumer");
                        if(!nr::interop::reusable(closed))completed=false;
                    } else require(owner.bridge->closed() && owner.ending,
                        "stop requires genuine accepted original Bridge close");
                    // Retain the original fence/receipt after accepted close erases keys.
                    // A higher shared value never substitutes for an ending receipt.
                    if(!owner.ending || nr::interop::completion(owner.ending->completionValue,
                        owner.releaseFence->GetCompletedValue())!=nr::interop::Completion::Complete)
                        completed=false;
                }
                if(completed) {
                    owners->clear();
                }
            }
            if(!s.slots.empty() || !s.retired.empty())return;
            for(size_t lane=0;lane<2;++lane)if(s.scopes[lane].filter && !s.sourceClosed[lane]) {
                calldata_t close;calldata_init(&close);
                require(proc_handler_call(obs_source_get_proc_handler(s.scopes[lane].filter),"nr_native_stop",&close),
                    "capacity original graphics close");
                const bool closed=calldata_bool(&close,"closed");calldata_free(&close);
                if(closed) {
                    if(s.nativeOriginal[lane]) {
                        calldata_init(&close);calldata_set_ptr(&close,"snapshot",&s.closure[lane]);
                        require(proc_handler_call(obs_get_proc_handler(),"nr_retirement_snapshot",&close),"capacity primitive exact closure");
                        calldata_free(&close);
                        require(s.closure[lane].instance==s.instance[lane] &&
                            nr::lifecycle::close_complete(s.closure[lane]),"capacity wrong or incomplete closure");
                    }
                    s.sourceClosed[lane]=true;
                }
            }
            if(capacity_closed())SetEvent(s.closed);
            if(s.failed || WaitForSingleObject(s.trigger,0)!=WAIT_OBJECT_0)return;
        }
        const bool waitingEnvelope=s.waiting;
        if(waitingEnvelope) {
            require(s.waitingFrames<64,"bounded retirement recovery observation envelopes");
            s.fixtures[1]->content=9+s.waitingFrames%6;
            s.armed[1]=s.observations[1]->arm();
            ++s.waitingFrames;
        } else {
            if(WaitForSingleObject(s.trigger,0)!=WAIT_OBJECT_0)return;
            ResetEvent(s.trigger);
        }
        const char* names[]{"slot-prime","slot-hold1","slot-hold2","slot-full-first","slot-full-repeat",
            "slot-recovery-first","slot-recovery-selected","retire-gen1","hold-gen2","retire-gen2",
            "budget-full-first","budget-full-repeat","budget-recovery-first","budget-recovery-selected",
            "stop-request","closed-originals"};
        std::array<uint64_t,2> baselineHash{},baselineTick{};
        std::array<std::optional<nr::JobKey>,2> baselineJob;
        for(unsigned view=0;view<3;++view)for(unsigned order=0;order<2;++order) {
            const auto lane=(s.phase%2+order)%2;
            if(waitingEnvelope && lane==0)continue;
            auto& scope=s.scopes[lane];
            calldata_t before;calldata_init(&before);
            require(proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_sampler_snapshot",&before),"capacity pre-view counters");
            const auto ingressBefore=calldata_int(&before,"ingress");calldata_free(&before);
            const auto rendersBefore=s.fixtures[lane]->renders;
            const auto image=lifecycle_view(scope.views[view],false,1920);
            const auto imageHash=nr::qa::bootstrap::hash(image);
            const auto imageId=lifecycle_image_id(image,false,1920);
            calldata_t sample;calldata_init(&sample);
            require(proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_sampler_snapshot",&sample),"capacity sampler state");
            std::optional<nr::obs_adapter::TickWitness> witness;
            nr::pipeline::Counters counts{};
            std::shared_ptr<nr::model::Job> job,selected;
            std::shared_ptr<const nr::obs_adapter::Capture> raw,current;
            std::shared_ptr<nr::interop::Bridge> bridge;
            calldata_t native;calldata_init(&native);
            calldata_set_ptr(&native,"tick_witness",&witness);calldata_set_ptr(&native,"counters",&counts);
            calldata_set_ptr(&native,"retain_job",&job);calldata_set_ptr(&native,"retain_selected_job",&selected);
            calldata_set_ptr(&native,"retain_raw",&raw);calldata_set_ptr(&native,"retain_current_raw",&current);
            calldata_set_ptr(&native,"retain_bridge",&bridge);
            require(proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_native_snapshot",&native) &&
                !calldata_bool(&native,"failed"),"capacity native state failed");
            const auto tick=nr::sampler::canonical_tick(obs_get_video_frame_time(),obs_get_frame_interval_ns());
            const auto id=static_cast<uint64_t>(calldata_int(&sample,"instance"));
            const auto ingress=static_cast<uint64_t>(calldata_int(&sample,"ingress"));
            const std::string state=calldata_string(&native,"public_state"),reasonText=calldata_string(&native,"public_reason");
            const bool stopping=s.stopping;
            const auto kind=stopping?nr::OutputKind::CurrentSource:static_cast<nr::OutputKind>(calldata_int(&sample,"kind"));
            const auto reason=stopping?nr::Reason::Cancelled:kind==nr::OutputKind::CurrentSource && witness &&
                witness->tick==tick?witness->admissionReason:static_cast<nr::Reason>(calldata_int(&sample,"lifecycle_reason"));
            const bool admitted=witness && witness->tick==tick && witness->admitted.has_value();
            require(id && (!admitted || (job && job->contract.key==*witness->admitted)),"capacity current admission identity");
            if(!view) {
                s.instance[lane]=id;baselineHash[lane]=imageHash;baselineTick[lane]=tick;
                if(admitted) {
                    baselineJob[lane].emplace(*witness->admitted);
                    require(s.receiptCount[lane]<80,"bounded actual capacity jobs");
                    s.receipts[lane][s.receiptCount[lane]++].emplace(Capacity::Receipt{
                        *witness->admitted,tick,s.armed[lane],s.phase,waitingEnvelope});
                    s.latest[lane]=job;
                }
                if(bridge)s.bridges[lane]=bridge;
            } else require(baselineHash[lane]==imageHash && baselineTick[lane]==tick &&
                (admitted?baselineJob[lane]==witness->admitted:!baselineJob[lane]),
                "repeated capacity view changed pixels/tick/admission");
            require(lane?imageId>=9 && imageId<=14:imageId>=1 && imageId<=6,"capacity output contains foreign analytic source");
            if(!waitingEnvelope && !stopping && lane==0) {
                const bool slots=s.phase==3 || s.phase==4,budget=s.phase==10 || s.phase==11;
                if(slots || budget)require(!admitted && kind==nr::OutputKind::CurrentSource &&
                    state=="bypassed" && reason==(slots?nr::Reason::SlotsFull:nr::Reason::RetirementBudget) &&
                    reasonText==(slots?"slotsFull":"retirementBudget") && imageId==s.fixtures[lane]->content,
                    "production capacity refusal not explicit current-source bypass");
                else require(admitted,"A remained arbitrarily refused after real capacity recovery");
                if(budget)require(ingress==static_cast<uint64_t>(ingressBefore) &&
                    calldata_int(&sample,"resource_generation")==3 && calldata_int(&native,"retired_generations")==2,
                    "retirement refusal captured or allocated generation four");
                if(s.phase==7)require(calldata_int(&sample,"resource_generation")==2 &&
                    calldata_int(&native,"retired_generations")==1,"first actual retired generation missing");
                if(s.phase==9)require(calldata_int(&sample,"resource_generation")==3 &&
                    calldata_int(&native,"retired_generations")==2,"second actual retired generation missing");
                if(s.phase==12 || s.phase==13)require(calldata_int(&sample,"resource_generation")==4 &&
                    calldata_int(&native,"retired_generations")==0,"same-instance retirement recovery not collected");
            }
            if(lane==1 && !stopping) {
                require(admitted,"A exhaustion refused independent B");
                if(!s.independentB)s.independentB.emplace(nr::Epochs{id,
                    witness->admitted->frame.contentEpoch,witness->admitted->historyEpoch,witness->admitted->resourceGeneration});
                require(admitted && witness->admitted->frame.contentEpoch==s.independentB->contentEpoch &&
                    witness->admitted->historyEpoch==s.independentB->historyEpoch &&
                    calldata_int(&sample,"resource_generation")==1 &&
                    calldata_int(&native,"retired_generations")==0 && state!="error" &&
                    reason!=nr::Reason::SlotsFull && reason!=nr::Reason::RetirementBudget,
                    "A exhaustion affected independent B admission/epochs");
            }
            if((s.phase==6 || s.phase==13) && lane==0 && !waitingEnvelope)
                require(kind==nr::OutputKind::DelayedNr && selected && raw &&
                    selected->contract.key.frame==raw->input.key && selected->contract.input.canonicalTick+1==tick,
                    "recovered A selected wrong exact predecessor");
            uint64_t rawHash=0;
            if(raw) {
                require(raw->input.key.instanceId==id,"capacity foreign selected raw owner");
                const auto rawImage=nr_bootstrap_read_texture(raw->original11.Get(),false,1920,1080);
                rawHash=nr::qa::bootstrap::hash(rawImage);
                if(kind!=nr::OutputKind::CurrentSource)require(rawHash==imageHash,"native copy output differs from exact raw phase");
            }
            require(calldata_int(&native,"retired_generations")<=2,"native allocated retired generation three");
            if(!waitingEnvelope && s.phase==3 && lane==0 && view==0 && s.slots.size()==2)
                capacity_hold(s,s.slots,s.latest[0],bridge);
            const auto& last=s.receipts[lane][s.receiptCount[lane]-1]->key;
            const auto contentEpoch=stopping?last.frame.contentEpoch:static_cast<uint64_t>(calldata_int(&sample,"content_epoch"));
            const auto historyEpoch=stopping?last.historyEpoch:static_cast<uint64_t>(calldata_int(&sample,"history_epoch"));
            const auto resourceGeneration=stopping?last.resourceGeneration:static_cast<uint64_t>(calldata_int(&sample,"resource_generation"));
            std::ostringstream record;
            record<<"NR_INSTANCE_CAPACITY_VIEW {\"cohort\":\"native-transport-capacity\",\"phase\":"
                <<(waitingEnvelope?-1:static_cast<int>(s.phase))<<",\"case\":\""<<(waitingEnvelope?"retirement-recovery-wait":names[s.phase])
                <<"\",\"lane\":"<<lane<<",\"view\":"<<view<<",\"recovery_wait\":"<<(waitingEnvelope?"true":"false")
                <<",\"instance\":"<<id<<",\"tick\":"<<tick<<",\"ingress_before\":"<<ingressBefore
                <<",\"ingress\":"<<ingress<<",\"current_admitted\":"<<(admitted?"true":"false")<<",\"current_job\":";
            capacity_key(record,admitted?&*witness->admitted:nullptr);
            record<<",\"selected\":"<<(kind==nr::OutputKind::CurrentSource?0:calldata_int(&sample,"selected"))
                <<",\"kind\":"<<static_cast<unsigned>(kind)<<",\"stored_tick\":"<<calldata_int(&sample,"tick")
                <<",\"stored_kind\":"<<calldata_int(&sample,"kind")<<",\"reason\":"<<static_cast<unsigned>(reason)
                <<",\"public_state\":\""<<state<<"\",\"public_reason\":\""<<reasonText
                <<"\",\"placeholder\":"<<(calldata_bool(&native,"placeholder")?"true":"false")
                <<",\"content_id\":"<<imageId<<",\"current_content_id\":"<<s.fixtures[lane]->content
                <<",\"output_hash\":\""<<imageHash<<"\",\"raw_hash\":\""<<(raw?std::to_string(rawHash):std::string{})
                <<"\",\"source_renders_before\":"<<rendersBefore<<",\"source_renders\":"<<s.fixtures[lane]->renders
                <<",\"content_epoch\":"<<contentEpoch<<",\"history_epoch\":"<<historyEpoch
                <<",\"resource_generation\":"<<resourceGeneration<<",\"epoch_source\":\""<<(stopping?"last_admitted":"live")<<"\""
                <<",\"retired_generations\":"<<calldata_int(&native,"retired_generations")
                <<",\"held_consumers\":"<<(lane?0:s.slots.size()+s.retired.size())
                <<",\"admission_reason\":"<<static_cast<unsigned>(admitted?witness->admissionReason:reason)
                <<",\"selected_job\":";
            capacity_key(record,kind==nr::OutputKind::DelayedNr && selected?&selected->contract.key:nullptr);
            record<<",\"counters\":{\"captures\":"<<counts.captures<<",\"dispatches\":"<<counts.dispatches
                <<",\"selections\":"<<counts.selections<<",\"collected_slots\":"<<counts.collectedSlots
                <<",\"collected_generations\":"<<counts.collectedGenerations<<"},\"slots\":[";
            for(size_t slot=0;slot<3;++slot) {
                const auto physical=(bridge?bridge:s.bridges[lane])->ownership(slot);
                if(slot)record<<',';
                record<<"{\"slot\":"<<slot<<",\"key\":";capacity_key(record,physical.key?&*physical.key:nullptr);
                record<<",\"consumer_open\":"<<(physical.consumerOpen?"true":"false")
                    <<",\"producer\":"<<physical.producer<<",\"producer_done\":"<<physical.producerDone
                    <<",\"worker\":"<<physical.worker<<",\"worker_done\":"<<physical.workerDone
                    <<",\"consumer\":"<<physical.consumer<<",\"consumer_done\":"<<physical.consumerDone
                    <<",\"reusable\":"<<(nr::interop::reusable(physical)?"true":"false")<<'}';
            }
            record<<"]}\n";std::osyncstream(std::cout)<<record.str()<<std::flush;
            calldata_free(&native);calldata_free(&sample);
        }
        if(!waitingEnvelope && !s.stopping) {
            if(s.phase==1 || s.phase==2 || s.phase==6 || s.phase==8) {
                std::shared_ptr<nr::model::Job> aged;
                std::shared_ptr<nr::interop::Bridge> bridge;
                calldata_t native;calldata_init(&native);
                calldata_set_ptr(&native,"retain_selected_job",&aged);calldata_set_ptr(&native,"retain_bridge",&bridge);
                require(proc_handler_call(obs_source_get_proc_handler(s.scopes[0].filter),"nr_native_snapshot",&native),"actual aged capacity owner");
                calldata_free(&native);
                capacity_hold(s,s.phase<=3?s.slots:s.retired,aged,bridge);
            }
            if(s.phase==4 || s.phase==11) {
                if(s.phase==11) {
                    calldata_t watch;calldata_init(&watch);calldata_set_ptr(&watch,"event",s.recovered);
                    calldata_set_int(&watch,"min_generation",4);
                    require(proc_handler_call(obs_source_get_proc_handler(s.scopes[0].filter),"nr_qa_capacity_watch",&watch) &&
                        calldata_bool(&watch,"subscribed"),"pre-release actual generation transition subscription");
                    calldata_free(&watch);
                }
                // Controller precreated exclusive events. Bind the final target here,
                // immediately before Signal, after all intervening ordinary draws.
                for(auto& owner:s.phase==4?s.slots:s.retired) {
                    require(owner.releaseEvent && owner.releaseFence,"capacity release subscription missing");
                    owner.ending=owner.bridge->end_consumer(owner.consumer,owner.releaseEvent);
                    require(static_cast<bool>(owner.ending),"end exact original capacity consumer");
                }
                if(s.phase==11)s.waiting=true;
            }
        }
        if(capacity_closed())SetEvent(s.closed);
        if(!waitingEnvelope)SetEvent(s.rendered);
    }catch(const std::exception& error) {
        s.failed=true;std::osyncstream(std::cout)<<"NR_INSTANCE_CAPACITY_FAILURE "<<error.what()<<'\n'<<std::flush;
        SetEvent(s.rendered);SetEvent(s.recovered);
    }
}
void capacity_request_stop() {
    if(!retainedCapacity)return;
    auto& s=*retainedCapacity;
    obs_enter_graphics();
    try {
        if(!s.stopping) {
            for(size_t lane=0;lane<2;++lane)if(s.scopes[lane].filter) {
                std::shared_ptr<nr::interop::Context> context;
                calldata_t data;calldata_init(&data);
                require(proc_handler_call(obs_source_get_proc_handler(s.scopes[lane].filter),"nr_sampler_snapshot",&data),
                    "pre-stop original capacity identity");
                s.instance[lane]=static_cast<uint64_t>(calldata_int(&data,"instance"));calldata_free(&data);
                calldata_init(&data);calldata_set_ptr(&data,"retain_exit_signal",&s.exited[lane]);
                calldata_set_ptr(&data,"retain_context",&context);
                require(proc_handler_call(obs_source_get_proc_handler(s.scopes[lane].filter),"nr_native_snapshot",&data),
                    "pre-stop original capacity worker subscription");
                calldata_free(&data);s.nativeOriginal[lane]=static_cast<bool>(context);
                require(!context || s.exited[lane],"original capacity worker exit signal unavailable");
                calldata_init(&data); // Unsubscribe the non-owning recovery HANDLE before any resource free.
                require(proc_handler_call(obs_source_get_proc_handler(s.scopes[lane].filter),"nr_qa_capacity_watch",&data),
                    "capacity recovery unsubscribe");
                calldata_free(&data);
            }
            s.latest={};s.waiting=false;s.stopping=true;
            if(!s.callbackAdded){obs_add_main_render_callback(capacity_draw,&s);s.callbackAdded=true;}
        }
    }catch(...){obs_leave_graphics();throw;}
    obs_leave_graphics();
}
void capacity_release() {
    if(!retainedCapacity)return;
    obs_enter_graphics();
    auto* s=retainedCapacity;
    if(capacity_closed()) {
        if(s->callbackAdded)obs_remove_main_render_callback(capacity_draw,s);
        retainedCapacity=nullptr;delete s;
    }
    obs_leave_graphics();
}
bool run_capacity(const std::filesystem::path& root,bool& retained) {
    auto* s=new Capacity;retainedCapacity=s;retained=true;
    bool graphics=false;
    try {
        require(s->trigger && s->rendered && s->closed && s->recovered,"pre-action capacity state subscriptions");
        obs_source_info info{};info.id="nr_task14_capacity_fixture";info.type=OBS_SOURCE_TYPE_INPUT;
        info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
        info.get_name=fixture_name;info.create=fixture_create;info.destroy=fixture_destroy;
        info.get_width=fixture_extent;info.get_height=fixture_height;info.video_render=fixture_render;info.video_get_color_space=fixture_space;
        obs_register_source(&info);
        for(size_t lane=0;lane<2;++lane) {
            auto& scope=s->scopes[lane];
            auto* data=obs_data_create();obs_data_set_bool(data,"nr_lifecycle_fixture",true);
            scope.source=obs_source_create_private(info.id,lane?"capacity source B":"capacity source A",data);
            obs_data_release(data);s->fixtures[lane]=fixture;
            require(scope.source && fixture,"actual capacity source");fixture->content=lane?9:1;
            data=obs_data_create();obs_data_set_string(data,"nr_shader_root",(root/"bin/64bit").string().c_str());
            obs_data_set_string(data,"nr_data_root",(root/"profile/ngx-data").string().c_str());
            obs_data_set_double(data,"nr_strength",lane?.75:.25);obs_data_set_bool(data,"nr_qa_deterministic",true);
            obs_data_set_bool(data,"nr_qa_capacity_evidence",true);
            scope.filter=obs_source_create_private("susemi_nr_filter",lane?"capacity filter B":"capacity filter A",data);
            obs_data_release(data);require(scope.filter!=nullptr,"actual capacity filter");obs_source_filter_add(scope.source,scope.filter);
            scope.scene=obs_scene_create_private(lane?"capacity program B":"capacity program A");
            require(scope.scene && obs_scene_add(scope.scene,scope.source),"actual repeated capacity source reference");
            for(size_t view=0;view<3;++view) {
                scope.views[view]=obs_view_create();require(scope.views[view]!=nullptr,"capacity actual OBS view");
                obs_view_set_source(scope.views[view],0,view==1?obs_scene_get_source(scope.scene):scope.source);
            }
            std::array<std::shared_ptr<nr::model::QaSignal>,3> signals;
            calldata_t native;calldata_init(&native);calldata_set_ptr(&native,"retain_dispatch_signals",&signals);
            require(proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_native_snapshot",&native),"module-owned capacity signals");
            calldata_free(&native);
            s->observations[lane]=std::make_shared<nr::qa::CapacityObservations>(std::move(signals));
            std::shared_ptr<nr::model::ObservationSink> sink=s->observations[lane];
            calldata_init(&native);calldata_set_ptr(&native,"observer",&sink);
            require(proc_handler_call(obs_source_get_proc_handler(scope.filter),"nr_native_observe",&native),"pre-trigger capacity sink");
            calldata_free(&native);
        }
        obs_add_main_render_callback(capacity_draw,s);s->callbackAdded=true;
        const auto complete=[&](size_t lane) {
            for(;s->consumed[lane]<s->receiptCount[lane];++s->consumed[lane]) {
                const auto& receipt=*s->receipts[lane][s->consumed[lane]];
                auto& observation=s->observations[lane]->at(receipt.envelope);
                require(observation.returned->wait_for(30000) &&
                    WaitForSingleObject(observation.observed,30000)==WAIT_OBJECT_0 &&
                    observation.finished.load(std::memory_order_acquire) && observation.key==receipt.key &&
                    observation.submitted && observation.completionFence &&
                    nr::interop::completion(observation.completionValue,observation.completionFence->GetCompletedValue())==
                        nr::interop::Completion::Complete,"actual capacity dispatch and physical copy completion");
                std::ostringstream record;
                record<<"NR_INSTANCE_CAPACITY_JOB {\"cohort\":\"native-transport-capacity\",\"lane\":"<<lane
                    <<",\"phase\":"<<(receipt.waiting?-1:static_cast<int>(receipt.phase))
                    <<",\"recovery_wait\":"<<(receipt.waiting?"true":"false")<<",\"instance\":"<<receipt.key.frame.instanceId
                    <<",\"tick\":"<<receipt.tick<<",\"key\":";capacity_key(record,&receipt.key);
                record<<",\"returned\":true,\"physical_complete\":true,\"transport_copy\":true,\"feature18_evaluated\":false}\n";
                std::osyncstream(std::cout)<<record.str()<<std::flush;
            }
        };
        const auto subscribe=[&](std::vector<Capacity::Held>& owners) {
            // Prepare only; acquisition serials are not fence completion targets.
            for(size_t i=0;i<owners.size();++i) {
                auto& owner=owners[i];
                const auto before=owner.bridge->ownership(owner.consumer.ticket.slot);
                require(before.key && *before.key==owner.consumer.ticket.key && before.consumerOpen &&
                    before.consumerLease==owner.consumer.leaseValue && !owner.ending &&
                    owner.releaseFence.Get()==owner.bridge->consumer_fence(),"original open consumer before release");
                owner.releaseEvent=nr::interop::ConsumerEvent::create();
                require(static_cast<bool>(owner.releaseEvent),"capacity release event");
            }
        };
        const auto release=[&](std::vector<Capacity::Held>& owners,const char* purpose) {
            for(size_t i=0;i<owners.size();++i) {
                require(owners[i].ending && owners[i].ending->event==owners[i].releaseEvent &&
                    WaitForSingleObject(owners[i].releaseEvent->handle(),30000)==WAIT_OBJECT_0,"actual consumer release completion");
                const auto& owner=owners[i];const auto actual=owner.releaseFence->GetCompletedValue();
                require(owner.ending->consumer.fence.Get()==owner.releaseFence.Get() &&
                    nr::interop::completion(owner.ending->completionValue,actual)==nr::interop::Completion::Complete &&
                    !owner.bridge->ownership(owner.consumer.ticket.slot).consumerOpen,"capacity consumer not physically ended");
                std::ostringstream record;record<<"NR_INSTANCE_CAPACITY_RELEASE {\"lane\":0,\"purpose\":\""<<purpose<<"\",\"key\":";
                capacity_key(record,&owner.consumer.ticket.key);
                record<<",\"slot\":"<<owner.consumer.ticket.slot<<",\"consumer_lease\":"<<owner.consumer.leaseValue
                    <<",\"required_consumer\":"<<owner.ending->completionValue
                    <<",\"actual_consumer\":"<<actual<<",\"event_observed\":true,\"consumer_open_before\":true,\"consumer_open_after\":false}\n";
                std::osyncstream(std::cout)<<record.str()<<std::flush;
            }
            owners.clear();
        };
        for(unsigned phase=0;phase<16;++phase) {
            if(phase==5)release(s->slots,"slots");
            if(phase==12) {
                release(s->retired,"retirement");
                require(WaitForSingleObject(s->recovered,30000)==WAIT_OBJECT_0 && !s->failed,"actual same-instance retirement recovery");
                obs_enter_graphics();graphics=true;s->waiting=false;obs_leave_graphics();graphics=false;
                complete(1);
            }
            obs_enter_graphics();graphics=true;s->phase=phase;
            if(phase==4)subscribe(s->slots);
            if(phase==11)subscribe(s->retired);
            for(size_t lane=0;lane<2;++lane) {
                s->fixtures[lane]->content=static_cast<uint32_t>((lane?9:1)+phase%6);
                if(phase<14 && !(lane==0 && (phase==10 || phase==11)))s->armed[lane]=s->observations[lane]->arm();
            }
            if(phase==7 || phase==9 || phase==10) {
                auto* settings=obs_source_get_settings(s->scopes[0].filter);
                obs_data_set_double(settings,"nr_strength",phase==9?.25:.5);obs_source_update(s->scopes[0].filter,settings);
                obs_data_release(settings);
            }
            if(phase==14) {
                for(size_t lane=0;lane<2;++lane) {
                    calldata_t native;calldata_init(&native);calldata_set_ptr(&native,"retain_exit_signal",&s->exited[lane]);
                    calldata_set_ptr(&native,"counters",&s->beforeStop[lane]);
                    require(proc_handler_call(obs_source_get_proc_handler(s->scopes[lane].filter),"nr_native_snapshot",&native),"pre-stop original capacity worker");
                    calldata_free(&native);
                }
                capacity_request_stop();
            }
            ResetEvent(s->rendered);SetEvent(s->trigger);obs_leave_graphics();graphics=false;
            require(WaitForSingleObject(s->rendered,15000)==WAIT_OBJECT_0 && !s->failed,"actual capacity view phase");
            if(phase<14){complete(0);if(phase!=11)complete(1);}
            obs_enter_graphics();graphics=true;
            if(phase==6 || phase==13) {
                std::optional<nr::obs_adapter::TickWitness> witness;
                std::shared_ptr<nr::model::Job> selected;
                calldata_t native;calldata_init(&native);calldata_set_ptr(&native,"tick_witness",&witness);
                calldata_set_ptr(&native,"retain_selected_job",&selected);
                require(proc_handler_call(obs_source_get_proc_handler(s->scopes[0].filter),"nr_native_snapshot",&native) &&
                    witness && witness->admitted && selected &&
                    witness->admitted->frame.instanceId==s->instance[0] &&
                    selected->contract.key.frame.instanceId==s->instance[0],"actual same-instance recovered keys");
                std::ostringstream record;
                record<<"NR_INSTANCE_CAPACITY_RECOVERY {\"purpose\":\""<<(phase==6?"slots":"retirement")
                    <<"\",\"instance\":"<<s->instance[0]<<",\"same_instance\":true,\"resource_generation\":"
                    <<witness->admitted->resourceGeneration<<",\"retired_generations\":"
                    <<calldata_int(&native,"retired_generations")<<",\"current_job\":";
                capacity_key(record,&*witness->admitted);record<<",\"selected_job\":";
                capacity_key(record,&selected->contract.key);
                record<<",\"subscription_before_release\":true,\"event_observed\":true}\n";
                std::osyncstream(std::cout)<<record.str()<<std::flush;calldata_free(&native);
            }
            obs_leave_graphics();graphics=false;
            if(phase==14) {
                for(const auto& event:s->exited)require(event && event->wait_for(30000),"exact original capacity worker exit");
            }
        }
        require(capacity_closed() && s->slots.empty() && s->retired.empty() &&
            s->receiptCount[0]==10 && s->receiptCount[1]==14+s->waitingFrames &&
            s->beforeStop[0].captures==12 && s->beforeStop[0].dispatches==10 &&
            s->beforeStop[1].captures==14+s->waitingFrames &&
            s->beforeStop[1].dispatches==14+s->waitingFrames,"capacity exact counts and all-owner closure");
        for(size_t lane=0;lane<2;++lane) {
            lifecycle_write_closure(s->closure[lane],"task14-capacity-closed",lane);
            std::osyncstream(std::cout)<<"NR_INSTANCE_CAPACITY_CLEANUP {\"lane\":"<<lane<<",\"instance\":"<<s->instance[lane]
                <<",\"closed\":true,\"worker_exit\":true,\"retained\":false,\"copy_jobs\":"<<s->receiptCount[lane]
                <<",\"capture_ingress\":"<<s->beforeStop[lane].captures
                <<",\"held_consumers\":0,\"recovery_wait_frames\":"<<(lane?s->waitingFrames:0)<<"}\n"<<std::flush;
        }
        obs_enter_graphics();graphics=true;obs_remove_main_render_callback(capacity_draw,s);
        s->callbackAdded=false;obs_leave_graphics();graphics=false;
        std::osyncstream(std::cout)<<"NR_INSTANCE_CAPACITY_SCOPE {\"fixed_view_rows\":96,\"recovery_wait_frames\":"<<s->waitingFrames
            <<",\"copy_jobs_a\":10,\"copy_jobs_b\":"<<s->receiptCount[1]
            <<",\"native_transport\":true,\"Feature18_capacity\":false,\"source_gate_only\":false}\n"<<std::flush;
        retained=false;retainedCapacity=nullptr;delete s;return true;
    }catch(const std::exception& error) {
        if(graphics)obs_leave_graphics();
        std::osyncstream(std::cout)<<"NR_INSTANCE_CAPACITY_FAILURE "<<error.what()<<"\nNR_INSTANCE_CAPACITY_RETAINED originals=true\n"<<std::flush;
        return false;
    }
}
#include "property_scenario.inc"
#include "fault_scenario.inc"
bool load_modules(const std::filesystem::path &root,bool bootstrap=false)
{
    obs_module_t *module = nullptr;
    auto plugin = (root / "obs-plugins/64bit/obs-nr-filter.dll").string();
    auto data = (root / "data/obs-plugins/obs-nr-filter").string();
    if (obs_open_module(&module, plugin.c_str(), data.c_str()) != MODULE_SUCCESS || !obs_init_module(module)) return false;
    obs_module_t *encoder = nullptr;
    auto encoderPath = (root / "obs-plugins/64bit/obs-x264.dll").string();
    auto encoderData = (root / "data/obs-plugins/obs-x264").string();
    if (obs_open_module(&encoder, encoderPath.c_str(), encoderData.c_str()) != MODULE_SUCCESS || !obs_init_module(encoder)) return false;
    if(bootstrap)for(const auto* name:{"obs-nvenc","obs-ffmpeg"}) {
        obs_module_t* recording=nullptr;
        const auto binary=(root/"obs-plugins/64bit"/(std::string(name)+".dll")).string();
        const auto assets=(root/"data/obs-plugins"/name).string();
        if(obs_open_module(&recording,binary.c_str(),assets.c_str())!=MODULE_SUCCESS || !obs_init_module(recording))return false;
    }
    const char *id = nullptr;
    for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) if (std::string(id) == "obs_x264") return true;
    return false;
}
}

int wmain(int argc, wchar_t **argv)
{
    nr_host_install_logger();
    if(argc==2&&std::wstring_view(argv[1])==L"--task26-storage-metadata")
        return nr_ghost_storage_metadata()?0:1;
    if(argc==4&&std::wstring_view(argv[1])==L"--task26-complete-service")
        return nr_ghost_complete_service(argv[2],argv[3])?0:1;
    if (argc != 7 && argc != 8 && argc != 9) return 64;
    const bool recording=argc==9 && std::wstring(argv[7])==L"recording-output";
    const bool bootstrap=argc==9 && std::wstring(argv[7])==L"native-bootstrap";
    const bool ghostBaseline=argc==9 && std::wstring(argv[7])==L"ghost-baseline";
    const bool ghost=argc==9 && (std::wstring(argv[7])==L"ghost-calibration"||ghostBaseline);
    const bool faults=argc==9 && std::wstring(argv[7])==L"handled-faults";
    const bool shutdown=argc==9 && std::wstring(argv[7])==L"shutdown";
    if(argc==9 && !bootstrap&&!ghost&&!recording&&!faults&&!shutdown)return 64;
    const bool sampler = argc == 8 && std::wstring(argv[7]) == L"sampler";
    const bool texture = argc == 8 && std::wstring(argv[7]) == L"texture-roundtrip";
    const bool flow = argc == 8 && std::wstring(argv[7]) == L"flow-translation";
    const bool color = argc == 8 && std::wstring(argv[7]) == L"color-alpha";
    const bool coreControl=argc==8 && std::wstring(argv[7])==L"core-init-control";
    const bool model = argc == 8 && (std::wstring(argv[7]) == L"native-model" || coreControl);
    const bool lifecycleNative=argc==8 && std::wstring(argv[7])==L"lifecycle-native";
    const bool lifecycle = argc == 8 && (std::wstring(argv[7]) == L"lifecycle" || lifecycleNative);
    const bool instances=argc==8 && std::wstring(argv[7])==L"two-instances";
    const bool propertySave=argc==8 && std::wstring(argv[7])==L"property-roundtrip-save";
    const bool propertyRestore=argc==8 && std::wstring(argv[7])==L"property-roundtrip-restore";
    const bool property=propertySave || propertyRestore;
    if (argc == 8 && !property && !sampler && !texture && !flow && !color && !model && !lifecycle && !instances) return 64;
    const std::filesystem::path root(argv[1]);

    HANDLE events[5]{};
    for (int i = 0; i != 5; ++i) {
        events[i] = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, argv[i + 2]);
        if (!events[i]) {
            for (auto event : events) if (event) CloseHandle(event);
            return 65;
        }
    }
    int result = 1;
    bool retained=false;
    std::unique_ptr<nr::qa::CoreControlOwner> coreOwner;
    HANDLE diagnosis[2]{};
    if(lifecycle || coreControl || instances || property || faults || shutdown) {
        for(size_t i=0;i<2;++i) {
            const auto name=std::wstring(argv[5])+(i?L".host-diagnosed":L".host-diagnose");
            diagnosis[i]=OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,name.c_str());
            if(!diagnosis[i])return 65;
        }
    }
    bool started = false;
    do {
        if (obs_get_version() != LIBOBS_API_VER) break;
        auto profile = (root / "profile").string();
        started = obs_startup("en-US", profile.c_str(), nullptr);
        if (!started) break;
        obs_video_info video{};
        auto graphics = (root / "bin/64bit/libobs-d3d11.dll").string();
        video.graphics_module = graphics.c_str();
        // Finite identity/ownership fixture, not Task18's performance workload.
        video.fps_num = model || bootstrap || recording || ghost || lifecycle || instances || property || faults || shutdown ? 1 : 60;
        video.fps_den = 1;
        video.base_width = video.output_width = 64;
        video.base_height = video.output_height = 64;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709;
        video.range = VIDEO_RANGE_FULL;
        video.scale_type = OBS_SCALE_DISABLE;
        if (color || model || bootstrap || recording || ghost || instances) {
            try { nr_color_refuse_unrelated(); }
            catch (const std::exception& error) { std::cout<<"NR_COLOR_ADMISSION_FAILURE "<<error.what()<<std::endl; break; }
        }
        if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) break;
        if(bootstrap||ghost||recording) {
            std::cout<<"NR_BOOTSTRAP_VIDEO_LEVELS before_white="<<obs_get_video_sdr_white_level()
                <<" before_peak="<<obs_get_video_hdr_nominal_peak_level()<<std::endl;
            obs_set_video_levels(300,1000); // Official defaults, owned headless libobs only.
        }
        if (!load_modules(root,bootstrap||ghost||recording)) break;
        std::cout << "NR_QA_READY libobs=" << obs_get_version_string() << " module=obs-nr-filter encoder=obs_x264\n" << std::flush;
        SetEvent(events[0]);
        if (WaitForSingleObject(events[1], 15000) != WAIT_OBJECT_0) break;
        if (sampler && !run_sampler()) break;
        if (texture && !nr_run_interop_scenario(root)) break;
        if (flow && !nr_run_flow_scenario(root)) break;
        if (color && !nr_run_color_scenario(root)) break;
        if(coreControl) {
            bool valid=false;
            try{valid=nr_start_core_control(root,coreOwner);}
            catch(const std::exception& error){std::cout<<"NR_CORE_CONTROL_FAILURE "<<error.what()<<std::endl;}
            if(coreOwner) {
                core_control_receipt(*coreOwner,"after-first-capture",0);
                retained=true;
                try {
                    if(coreOwner->request(1))(void)coreOwner->wait(15000);
                    retained=!core_control_receipt(*coreOwner,"post-close",0);
                    if(!retained)retained=!coreOwner->detach();
                    if(!retained)core_control_receipt(*coreOwner,"normal-close",0);
                }catch(const std::exception& error){retained=true;std::cout<<"NR_CORE_CONTROL_FAILURE "<<error.what()<<std::endl;}
            }
            if(!valid || retained)break;
        } else if (model && !nr_run_model_scenario(root,retained)) break;
        if(lifecycle && !run_lifecycle(root,retained,lifecycleNative))break;
        if(instances && !run_instances(root,retained))break;
        if(instances && !run_capacity(root,retained))break;
        if(property && !run_property(root,retained,propertyRestore))break;
        if(faults && !run_faults(root,retained,std::filesystem::path(argv[8])))break;
        if(shutdown && !run_shutdown(root,retained))break;
        if(bootstrap) {
            if(!nr_run_slot_scenario(root,retained))break;
            if(!nr_run_bootstrap_scenario(root,std::filesystem::path(argv[8]),retained))break;
        }
        if(recording&&!nr_run_recording_scenario(root,std::filesystem::path(argv[8]),retained))break;
        if(ghost&&!(ghostBaseline?nr_run_ghost_baseline(root,std::filesystem::path(argv[8]),retained):nr_run_ghost_scenario(root,std::filesystem::path(argv[8]),retained)))break;
        if (sampler) {
            // Separate finite real unsupported-backend lane, not an injected admission hook.
            // obs_reset_video does not recreate the graphics backend; end the first libobs session.
            obs_shutdown(); started = false;
            started = obs_startup("en-US", profile.c_str(), nullptr);
            if (!started) break;
            graphics = (root / "bin/64bit/libobs-opengl.dll").string();
            video.graphics_module = graphics.c_str();
            if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS || !load_modules(root) || !run_sampler(true)) break;
        }
        auto *settings = obs_data_create();
        obs_data_set_int(settings, "content_id", 0xA17);
        const bool same = obs_data_get_int(settings, "content_id") == 0xA17;
        obs_data_release(settings);
        if (!same) break;
        std::cout << "NR_QA_ACTION content_id=2583\n" << std::flush;
        SetEvent(events[2]);
        if (WaitForSingleObject(events[3], 15000) != WAIT_OBJECT_0) break;
        result = 0;
    } while (false);
    if(lifecycle || shutdown)retained=!lifecycle_closure("post-source-destruction") || retained;
    retained=nr::qa::recording_has_uncertain_owners() || retained;
    retained=nr::qa::fold_model_retained(retained);
    if(retained) {
        std::cout<<"NR_QA_RETAINED native ownership unresolved; module/device/process kept loaded"<<std::endl;
        if(faults)SetEvent(events[2]); // Diagnostic checkpoint, not normal closure.
        const bool normal=!nr::qa::recording_has_uncertain_owners() && !nr::qa::model_has_unresolved_owners() && (coreControl && coreOwner?
            supervise_core_control(*coreOwner,diagnosis[0],diagnosis[1],events[0]):
            (lifecycle || instances || property || faults || shutdown) && supervise_retained(diagnosis[0],diagnosis[1],events[0]));
        if(!normal) {
            for(;;) {
                WaitForSingleObject(events[1],INFINITE);
                ResetEvent(events[1]);
                std::cout<<"NR_QA_RETAINED unsupported cooperative diagnosis; ownership retained"<<std::endl;
                SetEvent(events[2]);
            }
        }
        if(coreControl)core_control_receipt(*coreOwner,"retained-normal-close",0);
        else if(lifecycle)lifecycle_closure("retained-normal-close");
        result=70;
    }
    if(coreControl && coreOwner && coreOwner->snapshot().closed)coreOwner.reset();
    if (started) obs_shutdown();
    for(auto event:diagnosis)if(event)CloseHandle(event);
    std::cout << "NR_QA_EXIT cleanup=complete result=" << result << "\n" << std::flush;
    SetEvent(events[4]);
    for (auto event : events) CloseHandle(event);
    return result;
}
