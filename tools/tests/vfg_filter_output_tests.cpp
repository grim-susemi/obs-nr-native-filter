// SPDX-License-Identifier: GPL-3.0-only
#include <obs-module.h>
#include <graphics/vec4.h>
#include <windows.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t width = 320, height = 180;
constexpr char filterId[] = "susemi_fg_tap";
using Pixels = std::array<uint8_t, 4>;
constexpr Pixels red{255, 0, 0, 255}, blue{0, 0, 255, 255}, transparent{};

void expect(bool ok, const char* why)
{
    if (!ok) throw std::runtime_error(why);
}

auto settings() { return std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(obs_data_create(), obs_data_release); }
auto properties(obs_source_t* source)
{
    return std::unique_ptr<obs_properties_t, decltype(&obs_properties_destroy)>(
        obs_source_properties(source), obs_properties_destroy);
}
auto source(const char* id, obs_data_t* data = nullptr)
{
    auto result = std::unique_ptr<obs_source_t, decltype(&obs_source_release)>(
        obs_source_create_private(id, id, data), obs_source_release);
    expect(result && obs_obj_get_data(result.get()), "source creation");
    return result;
}

void quality_contract()
{
    const auto check = [](long long legacy, long long expected, bool newer = false) {
        auto values = settings();
        obs_data_set_int(values.get(), "vfg_quality", legacy);
        if (newer) obs_data_set_int(values.get(), "vfg_quality_mode", expected);
        auto filter = source(filterId, values.get());
        obs_source_load(filter.get());
        const auto saved = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(
            obs_source_get_settings(filter.get()), obs_data_release);
        expect(!obs_data_has_user_value(saved.get(), "vfg_quality") &&
            obs_data_get_int(saved.get(), "vfg_quality_mode") == expected, "legacy label meaning must survive reload");
    };
    auto filter = source(filterId);
    const auto defaults = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(
        obs_source_get_settings(filter.get()), obs_data_release);
    expect(obs_data_get_int(defaults.get(), "vfg_quality_mode") == 1, "Balanced is the default");
    const auto props = properties(filter.get());
    auto* mode = obs_properties_get(props.get(), "vfg_quality_mode");
    expect(mode && obs_property_list_item_count(mode) == 3, "three quality options");
    for (size_t i = 0; i < 3; ++i)
        expect(obs_property_list_item_int(mode, i) == static_cast<long long>(i), "Low/Medium/High UI values");
    check(0, 2);
    check(1, 1);
    check(2, 0);
    check(-1, 1);
    check(0, 0, true);
}

void render_fixture(void*, gs_effect_t*)
{
    auto* effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    const vec4 color{1.f, 0.f, 0.f, 1.f};
    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, width, height);
}

Pixels probe(obs_source_t* src, const Pixels& background, bool sourceOver)
{
    obs_enter_graphics();
    struct Leave { ~Leave() { obs_leave_graphics(); } } leave;
    const auto tex = std::unique_ptr<gs_texrender_t, decltype(&gs_texrender_destroy)>(
        gs_texrender_create(GS_RGBA, GS_ZS_NONE), gs_texrender_destroy);
    const auto stage = std::unique_ptr<gs_stagesurf_t, decltype(&gs_stagesurface_destroy)>(
        gs_stagesurface_create(width, height, GS_RGBA), gs_stagesurface_destroy);
    expect(tex && stage && gs_texrender_begin(tex.get(), width, height), "readback target");
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, sourceOver ? GS_BLEND_INVSRCALPHA : GS_BLEND_ZERO);
    gs_ortho(0, static_cast<float>(width), 0, static_cast<float>(height), -100, 100);
    const vec4 color{background[0] / 255.f, background[1] / 255.f, background[2] / 255.f, background[3] / 255.f};
    gs_clear(GS_CLEAR_COLOR, &color, 0, 0);
    obs_source_video_render(src);
    gs_blend_state_pop();
    gs_texrender_end(tex.get());
    gs_stage_texture(stage.get(), gs_texrender_get_texture(tex.get()));
    uint8_t* bytes{};
    uint32_t stride{};
    expect(gs_stagesurface_map(stage.get(), &bytes, &stride), "readback map");
    const auto* p = bytes + stride * (height / 2) + (width / 2) * 4;
    const Pixels out{p[0], p[1], p[2], p[3]};
    gs_stagesurface_unmap(stage.get());
    return out;
}

void canvas_contract(const std::filesystem::path& log)
{
    auto input = source("vfg_test_color");
    auto data = settings();
    obs_data_set_bool(data.get(), "vfg_hide_source_canvas", true);
    auto filter = source(filterId, data.get());
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    const auto props = properties(filter.get());
    expect(probe(input.get(), blue, false) == red, "before preview source is visible");
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_off"), filter.get()), "start original preview");
    expect(probe(input.get(), red, false) == transparent, "hidden draw must overwrite source-only RGBA");
    expect(probe(input.get(), blue, true) == blue, "hidden draw must preserve lower scene layers");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool publishing = false;
    while (!publishing && std::chrono::steady_clock::now() < deadline) {
        Sleep(25);
        std::ifstream stream(log);
        for (std::string line; std::getline(stream, line);) {
            if (line.starts_with("event=present_original ")) { publishing = true; break; }
        }
    }
    expect(publishing, "preview must receive originals while the canvas is hidden");
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()), "stop preview");
    expect(probe(input.get(), blue, true) == red, "stopping preview restores source");
    obs_queue_task(OBS_TASK_GRAPHICS, [](void*) {}, nullptr, true);
    obs_source_filter_remove(input.get(), filter.get());
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        expect(argc == 6, "arguments: plugin plugin-data presenter graphics-module scratch");
        const auto plugin = std::filesystem::path(argv[1]).string();
        const auto data = std::filesystem::path(argv[2]).string();
        const auto graphics = std::filesystem::path(argv[4]).string();
        const auto scratch = std::filesystem::path(argv[5]);
        std::filesystem::create_directories(scratch);
        const auto log = scratch / "canvas-presenter.log";
        std::filesystem::remove(log);
        SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", argv[3]);
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
        expect(obs_startup("en-US", scratch.string().c_str(), nullptr), "obs_startup");
        struct Shutdown { ~Shutdown() { obs_shutdown(); } } shutdown;
        obs_video_info video{};
        video.graphics_module = graphics.c_str();
        video.fps_num = 60; video.fps_den = 1;
        video.base_width = video.output_width = width;
        video.base_height = video.output_height = height;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709;
        video.range = VIDEO_RANGE_FULL;
        expect(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "obs_reset_video");
        obs_module_t* module{};
        const auto pluginHandle = LoadLibraryW(argv[1]);
        expect(pluginHandle != nullptr, "retain plugin through libobs teardown");
        expect(obs_open_module(&module, plugin.c_str(), data.c_str()) == MODULE_SUCCESS &&
            obs_init_module(module), "load release plugin");
        obs_source_info info{};
        info.id = "vfg_test_color";
        info.type = OBS_SOURCE_TYPE_INPUT;
        info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
        info.get_name = [](void*) { return "VFG color fixture"; };
        info.create = [](obs_data_t*, obs_source_t* src) -> void* { return src; };
        info.destroy = [](void*) {};
        info.video_render = render_fixture;
        info.get_width = [](void*) { return width; };
        info.get_height = [](void*) { return height; };
        obs_register_source(&info);
        quality_contract();
        canvas_contract(log);
        std::puts("PASS vfg_filter_output: stored quality, RGBA zero, scene blend, preview feed and restore");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_filter_output: %s\n", e.what());
        return 1;
    }
}
