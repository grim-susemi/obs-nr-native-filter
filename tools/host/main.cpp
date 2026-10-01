// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include "nr_color_scenario.h"
#include "nr_model_scenario.h"
#include "nr_bootstrap_scenario.h"
#include "nr_slot_scenario.h"
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


namespace {

// Finite QA source. Production never maps textures or reads pixels back.
struct Fixture {
    gs_texture_t *texture = nullptr;
    unsigned renders = 0;
    uint32_t extent = 64;
    gs_color_space space = GS_CS_SRGB;
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
void *fixture_create(obs_data_t *, obs_source_t *) { fixture = new Fixture; return fixture; }
void fixture_destroy(void *data)
{
    auto *f = static_cast<Fixture *>(data);
    obs_enter_graphics(); gs_texture_destroy(f->texture); obs_leave_graphics();
    delete f; fixture = nullptr;
}
uint32_t fixture_extent(void *data) { return static_cast<Fixture *>(data)->extent; }
gs_color_space fixture_space(void *data, size_t, const gs_color_space *) { return static_cast<Fixture *>(data)->space; }
const char *fixture_name(void *) { return "Task6 finite alpha/content fixture"; }
void fixture_render(void *data, gs_effect_t *)
{
    auto &f = *static_cast<Fixture *>(data);
    if (!f.extent) return;
    ++f.renders;
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
    if (argc != 7 && argc != 8 && argc != 9) return 64;
    const bool bootstrap=argc==9 && std::wstring(argv[7])==L"native-bootstrap";
    if(argc==9 && !bootstrap)return 64;
    const bool sampler = argc == 8 && std::wstring(argv[7]) == L"sampler";
    const bool texture = argc == 8 && std::wstring(argv[7]) == L"texture-roundtrip";
    const bool flow = argc == 8 && std::wstring(argv[7]) == L"flow-translation";
    const bool color = argc == 8 && std::wstring(argv[7]) == L"color-alpha";
    const bool model = argc == 8 && std::wstring(argv[7]) == L"native-model";
    if (argc == 8 && !sampler && !texture && !flow && !color && !model) return 64;
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
    HANDLE retainedOwner=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!retainedOwner)return 65;
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
        video.fps_num = model || bootstrap ? 1 : 60;
        video.fps_den = 1;
        video.base_width = video.output_width = 64;
        video.base_height = video.output_height = 64;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709;
        video.range = VIDEO_RANGE_FULL;
        video.scale_type = OBS_SCALE_DISABLE;
        if (color || model || bootstrap) {
            try { nr_color_refuse_unrelated(); }
            catch (const std::exception& error) { std::cout<<"NR_COLOR_ADMISSION_FAILURE "<<error.what()<<std::endl; break; }
        }
        if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) break;
        if(bootstrap) {
            std::cout<<"NR_BOOTSTRAP_VIDEO_LEVELS before_white="<<obs_get_video_sdr_white_level()
                <<" before_peak="<<obs_get_video_hdr_nominal_peak_level()<<std::endl;
            obs_set_video_levels(300,1000); // Official defaults, owned headless libobs only.
        }
        if (!load_modules(root,bootstrap)) break;
        std::cout << "NR_QA_READY libobs=" << obs_get_version_string() << " module=obs-nr-filter encoder=obs_x264\n" << std::flush;
        SetEvent(events[0]);
        if (WaitForSingleObject(events[1], 15000) != WAIT_OBJECT_0) break;
        if (sampler && !run_sampler()) break;
        if (texture && !nr_run_interop_scenario(root)) break;
        if (flow && !nr_run_flow_scenario(root)) break;
        if (color && !nr_run_color_scenario(root)) break;
        if (model && !nr_run_model_scenario(root)) break;
        if(bootstrap) {
            if(!nr_run_slot_scenario(root,retained))break;
            if(!nr_run_bootstrap_scenario(root,std::filesystem::path(argv[8]),retained))break;
        }
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
    if(retained) {
        std::cout<<"NR_QA_RETAINED native ownership unresolved; module/device/process kept loaded"<<std::endl;
        WaitForSingleObject(retainedOwner,INFINITE); // Ownership keeper, NOT a successful bounded shutdown.
        return 70;
    }
    if (started) obs_shutdown();
    CloseHandle(retainedOwner);
    std::cout << "NR_QA_EXIT cleanup=complete result=" << result << "\n" << std::flush;
    SetEvent(events[4]);
    for (auto event : events) CloseHandle(event);
    return result;
}
