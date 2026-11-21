// SPDX-License-Identifier: GPL-3.0-only
#include <obs-module.h>
#include <graphics/vec4.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t width = 640, height = 360;
using Pixel = std::array<uint8_t, 4>;
using Image = std::vector<Pixel>;
void expect(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
auto data() { return std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(obs_data_create(), obs_data_release); }
auto source(const char* id, obs_data_t* values = nullptr)
{
    auto result = std::unique_ptr<obs_source_t, decltype(&obs_source_release)>(
        obs_source_create_private(id, id, values), obs_source_release);
    expect(result && obs_obj_get_data(result.get()), "create source");
    return result;
}
void fixture(void*, gs_effect_t*)
{
    auto* effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    for (uint32_t x = 0; x < width; x += 4) {
        const float value = (x / 4 % 3 == 0 ? 0.25f : x / 4 % 3 == 1 ? 0.45f : 0.65f);
        const vec4 color{value, value, value, 1.0f};
        gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
        gs_matrix_push();
        gs_matrix_translate3f(static_cast<float>(x), 0, 0);
        while (gs_effect_loop(effect, "Solid")) gs_draw_sprite(nullptr, 0, 4, height);
        gs_matrix_pop();
    }
}
void translucent_fixture(void*, gs_effect_t*)
{
    auto* effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    const vec4 color{0.20f, 0.0f, 0.0f, 0.5f};
    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    while (gs_effect_loop(effect, "Solid")) gs_draw_sprite(nullptr, 0, width, height);
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
}
Image render(obs_source_t* input, vec4 background = {})
{
    obs_enter_graphics();
    struct Leave { ~Leave() { obs_leave_graphics(); } } leave;
    const auto target = std::unique_ptr<gs_texrender_t, decltype(&gs_texrender_destroy)>(
        gs_texrender_create(GS_RGBA, GS_ZS_NONE), gs_texrender_destroy);
    const auto stage = std::unique_ptr<gs_stagesurf_t, decltype(&gs_stagesurface_destroy)>(
        gs_stagesurface_create(width, height, GS_RGBA), gs_stagesurface_destroy);
    expect(target && stage && gs_texrender_begin(target.get(), width, height), "render target");
    gs_ortho(0, static_cast<float>(width), 0, static_cast<float>(height), -100, 100);
    gs_clear(GS_CLEAR_COLOR, &background, 0, 0);
    obs_source_video_render(input);
    gs_texrender_end(target.get());
    gs_stage_texture(stage.get(), gs_texrender_get_texture(target.get()));
    uint8_t* bytes{};
    uint32_t stride{};
    expect(gs_stagesurface_map(stage.get(), &bytes, &stride), "map pixels");
    Image result(width * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            std::copy_n(bytes + y * stride + x * 4, 4, result[y * width + x].begin());
    gs_stagesurface_unmap(stage.get());
    return result;
}
void configure(obs_source_t* filter, bool enabled, int position, bool swap = false)
{
    auto values = data();
    obs_data_set_bool(values.get(), "split_compare_enabled", enabled);
    obs_data_set_int(values.get(), "split_compare_position", position);
    obs_data_set_bool(values.get(), "split_compare_swap", swap);
    auto event = std::unique_ptr<void, decltype(&CloseHandle)>(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
    expect(event != nullptr, "create settings event");
    auto* signals = obs_source_get_signal_handler(filter);
    const auto updated = +[](void* handle, calldata_t*) { SetEvent(handle); };
    signal_handler_connect(signals, "update", updated, event.get());
    obs_source_update(filter, values.get());
    const auto signaled = WaitForSingleObject(event.get(), 3000);
    signal_handler_disconnect(signals, "update", updated, event.get());
    expect(signaled == WAIT_OBJECT_0, "OBS video settings update");
}
bool close(Pixel a, Pixel b)
{
    for (size_t i = 0; i < 4; ++i)
        if (std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])) > 3) return false;
    return true;
}
void compare(const Image& actual, const Image& before, const Image& after, int position, bool swap = false)
{
    const int divider = static_cast<int>(width) * position / 100;
    unsigned differing = 0, green = 0;
    for (int x = 4; x < static_cast<int>(width) - 4; ++x) {
        const auto index = (height / 2) * width + static_cast<uint32_t>(x);
        if (std::abs(x - divider) <= 3) {
            const auto p = actual[index];
            if (p[1] > 220 && p[0] < 40 && p[2] < 40) ++green;
            continue;
        }
        const bool showBefore = swap ? (x >= divider) : (x < divider);
        const auto expected = showBefore ? before[index] : after[index];
        if (!close(actual[index], expected)) {
            const auto seen = actual[index];
            std::fprintf(stderr, "split=%d x=%d actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n", position, x,
                seen[0], seen[1], seen[2], seen[3], expected[0], expected[1], expected[2], expected[3]);
            for (const auto sample : {width / 4, width / 2, 3 * width / 4}) {
                const auto p = actual[(height / 2) * width + sample];
                std::fprintf(stderr, "x=%u actual=%u,%u,%u,%u\n", sample, p[0], p[1], p[2], p[3]);
            }
            throw std::runtime_error("comparison side does not match original or processed pixels");
        }
        if (!close(before[index], after[index])) ++differing;
    }
    expect(differing > 10, "CAS fixture must distinguish before and processed");
    const auto edge = actual[(height / 2) * width + (position == 0 ? 0 : width - 1)];
    if (position == 0 || position == 100)
        expect(edge[1] > 220 && edge[0] < 40 && edge[2] < 40, "bright green divider must stay visible at either edge");
    else
        expect(green > 0, "bright green divider missing");
}
void check_defaults(obs_source_t* filter)
{
    const auto saved = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(
        obs_source_get_settings(filter), obs_data_release);
    expect(!obs_data_get_bool(saved.get(), "split_compare_enabled"), "split default off");
    expect(obs_data_get_int(saved.get(), "split_compare_position") == 50, "split default position");
    expect(!obs_data_get_bool(saved.get(), "split_compare_swap"), "split swap default off");
}
void run_chain(bool nrFirst)
{
    auto input = source("split_test_pattern");
    const Image before = render(input.get());
    auto values = data();
    obs_data_set_int(values.get(), "output_mode", 3);
    obs_data_set_int(values.get(), "cas_strength", 100);
    auto upscale = source("susemi_upscale_filter", values.get());
    auto nr = source("susemi_nr_filter");
    check_defaults(upscale.get());
    check_defaults(nr.get());
    auto* first = nrFirst ? nr.get() : upscale.get();
    auto* second = nrFirst ? upscale.get() : nr.get();
    obs_source_filter_add(input.get(), first);
    obs_source_filter_add(input.get(), second);
    const Image after = render(input.get());
    configure(upscale.get(), true, 50);
    compare(render(input.get()), before, after, 50);
    configure(upscale.get(), true, 0);
    compare(render(input.get()), before, after, 0);
    configure(upscale.get(), true, 100);
    compare(render(input.get()), before, after, 100);
    configure(upscale.get(), true, 40, true);
    compare(render(input.get()), before, after, 40, true);
    configure(upscale.get(), false, 50);
    configure(nr.get(), true, 50);
    compare(render(input.get()), before, after, 50);
    configure(first, true, 25);
    configure(second, true, 75);
    compare(render(input.get()), before, after, 75);
    configure(nr.get(), false, 50);
    configure(upscale.get(), false, 50);
    const auto off = render(input.get());
    for (size_t i = 0; i < off.size(); ++i)
        expect(close(off[i], after[i]), "split off must leave processed output unchanged");
    obs_source_filter_remove(input.get(), first);
    obs_source_filter_remove(input.get(), second);
}

void run_translucent()
{
    auto input = source("split_test_translucent");
    auto filter = source("susemi_nr_filter");
    obs_source_filter_add(input.get(), filter.get());
    const vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
    const auto baseline = render(input.get(), white);
    const auto left = height / 2 * width + width / 4;
    const auto right = height / 2 * width + width * 3 / 4;
    expect(baseline[left][0] >= 215 && baseline[left][1] >= 185 && baseline[left][1] <= 190,
        "nonblack reference must blend in linear light");
    configure(filter.get(), true, 50);
    const auto split = render(input.get(), white);
    expect(close(split[left], baseline[left]) && close(split[right], baseline[right]),
        "both split sides must preserve linear SDR blending over the canvas");
    obs_source_filter_remove(input.get(), filter.get());
}

void run_active_nr(const std::filesystem::path& shaderDir, const std::filesystem::path& scratch, const char* runtime, obs_video_info video)
{
    auto values = data();
    const auto path = shaderDir.string(), profile = (scratch / "ngx-data").string();
    obs_data_set_string(values.get(), "nr_runtime_path", runtime);
    obs_data_set_string(values.get(), "nr_shader_root", path.c_str());
    obs_data_set_string(values.get(), "nr_data_root", profile.c_str());
    obs_data_set_bool(values.get(), "split_compare_enabled", true);
    auto input = source("split_test_translucent");
    auto filter = source("susemi_nr_filter", values.get());
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    bool active = false;
    const auto until = GetTickCount64() + 10000;
    while (GetTickCount64() < until && !active) {
        obs_enter_graphics();
        calldata_t snapshot{};
        calldata_init(&snapshot);
        const bool called = proc_handler_call(obs_source_get_proc_handler(filter.get()), "nr_sampler_snapshot", &snapshot);
        active = called && calldata_int(&snapshot, "selected") > 0;
        calldata_free(&snapshot);
        obs_leave_graphics();
        if (!active)
            Sleep(16);
    }
    expect(active, "active NR pipeline must select a frame");
    const auto pixels = render(input.get(), {1.0f, 1.0f, 1.0f, 1.0f});
    const auto right = pixels[(height / 2) * width + width * 3 / 4];
    expect(right[1] >= 175 && right[1] <= 200 && right[0] > right[1] + 8 && right[3] >= 250,
        "active NR split must retain nonblack color and linear over for half-alpha output");
    std::printf("PASS active NR translucent split (right RGB %u,%u,%u)\n", right[0], right[1], right[2]);
    expect(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "obs_reset_video with active NR filter");
    bool recovered = false;
    const auto recoveryUntil = GetTickCount64() + 10000;
    while (GetTickCount64() < recoveryUntil && !recovered) {
        obs_enter_graphics();
        calldata_t snapshot{};
        calldata_init(&snapshot);
        const bool called = proc_handler_call(obs_source_get_proc_handler(filter.get()), "nr_sampler_snapshot", &snapshot);
        const auto selected = called ? calldata_int(&snapshot, "selected") : 0;
        calldata_free(&snapshot);
        calldata_t nativeSnap{};
        calldata_init(&nativeSnap);
        const bool nativeCalled = proc_handler_call(obs_source_get_proc_handler(filter.get()), "nr_native_snapshot", &nativeSnap);
        const bool failed = nativeCalled ? calldata_bool(&nativeSnap, "failed") : true;
        calldata_free(&nativeSnap);
        obs_leave_graphics();
        expect(!failed, "active NR must not fail on obs_reset_video");
        recovered = selected > 0;
        if (!recovered) Sleep(16);
    }
    expect(recovered, "active NR pipeline must recover and select a frame after obs_reset_video");
    const auto postResetPixels = render(input.get(), {1.0f, 1.0f, 1.0f, 1.0f});
    const auto postRight = postResetPixels[(height / 2) * width + width * 3 / 4];
    expect(postRight[1] >= 175 && postRight[1] <= 200 && postRight[0] > postRight[1] + 8 && postRight[3] >= 250,
        "active NR split after obs_reset_video must retain nonblack color");
    std::printf("PASS active NR obs_reset_video recovery (right RGB %u,%u,%u)\n", postRight[0], postRight[1], postRight[2]);
    obs_source_filter_remove(input.get(), filter.get());
}
}
int wmain(int argc, wchar_t** argv)
{
    try {
        expect(argc == 5, "arguments: plugin plugin-data graphics-module scratch");
        const auto plugin = std::filesystem::path(argv[1]).string();
        const auto dataPath = std::filesystem::path(argv[2]).string();
        const auto graphics = std::filesystem::path(argv[3]).string();
        const auto scratch = std::filesystem::path(argv[4]);
        std::filesystem::create_directories(scratch);
        expect(obs_startup("en-US", scratch.string().c_str(), nullptr), "obs_startup");
        struct Shutdown { ~Shutdown() { obs_shutdown(); } } shutdown;
        obs_video_info video{};
        video.graphics_module = graphics.c_str();
        video.fps_num = 60; video.fps_den = 1;
        video.base_width = video.output_width = width;
        video.base_height = video.output_height = height;
        video.output_format = VIDEO_FORMAT_RGBA; video.colorspace = VIDEO_CS_709; video.range = VIDEO_RANGE_FULL;
        expect(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "obs_reset_video");
        expect(LoadLibraryW(argv[1]) != nullptr, "retain plugin");
        obs_module_t* module{};
        expect(obs_open_module(&module, plugin.c_str(), dataPath.c_str()) == MODULE_SUCCESS &&
            obs_init_module(module), "load plugin");
        obs_source_info info{};
        info.id = "split_test_pattern"; info.type = OBS_SOURCE_TYPE_INPUT;
        info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
        info.get_name = [](void*) { return "split pattern"; };
        info.create = [](obs_data_t*, obs_source_t* src) -> void* { return src; };
        info.destroy = [](void*) {};
        info.video_render = fixture;
        info.get_width = [](void*) { return width; };
        info.get_height = [](void*) { return height; };
        obs_register_source(&info);
        info.id = "split_test_translucent";
        info.video_render = translucent_fixture;
        obs_register_source(&info);
        run_chain(true);
        run_chain(false);
        run_translucent();
        char runtime[4096]{};
        const auto length = GetEnvironmentVariableA("OBS_NR_SPLIT_TEST_RUNTIME", runtime, ARRAYSIZE(runtime));
        if (length && length < ARRAYSIZE(runtime))
            run_active_nr(std::filesystem::path(argv[1]).parent_path().parent_path(), scratch, runtime, video);
        std::puts("PASS split_filter_output");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL split_filter_output: %s\n", e.what());
        return 1;
    }
}
