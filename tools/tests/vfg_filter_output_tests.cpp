// SPDX-License-Identifier: GPL-3.0-only
#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/text-lookup.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

std::filesystem::path vfg_stage_anr_addon(const std::filesystem::path& fixture,
    const std::filesystem::path& addon, bool stageCompanions);

namespace {
constexpr uint32_t width = 320, height = 180;
std::atomic_uint32_t fixtureWidth{width};
std::atomic_bool fixtureBlue{};
std::atomic_bool fixtureAnimate{};
std::atomic_bool fixturePacing1750{};
std::atomic_uint32_t fixtureFrameSeq{};
constexpr char filterId[] = "susemi_fg_tap";
using Pixels = std::array<uint8_t, 4>;
constexpr Pixels red{255, 0, 0, 255}, blue{0, 0, 255, 255}, transparent{};

void expect(bool ok, const char* why)
{
    if (!ok) throw std::runtime_error(why);
}

struct TapLogCapture {
    log_handler_t prior{};
    void* param{};
    mutable std::mutex guard;
    std::vector<std::string> lines;
    TapLogCapture()
    {
        base_get_log_handler(&prior, &param);
        base_set_log_handler([](int level, const char* format, va_list args, void* opaque) {
            auto& capture = *static_cast<TapLogCapture*>(opaque);
            va_list copy;
            va_copy(copy, args);
            char text[1024]{};
            const int written = vsnprintf(text, sizeof(text), format, copy);
            va_end(copy);
            if (written > 0) {
                std::lock_guard lock(capture.guard);
                capture.lines.emplace_back(text, std::min<size_t>(static_cast<size_t>(written), sizeof(text) - 1));
            }
            capture.prior(level, format, args, capture.param);
        }, this);
    }
    ~TapLogCapture() { base_set_log_handler(prior, param); }
    TapLogCapture(const TapLogCapture&) = delete;
    TapLogCapture& operator=(const TapLogCapture&) = delete;
    std::string last(const char* prefix) const
    {
        std::lock_guard lock(guard);
        for (auto it = lines.rbegin(); it != lines.rend(); ++it)
            if (it->starts_with(prefix)) return *it;
        return {};
    }
    std::vector<std::string> snapshot() const
    {
        std::lock_guard lock(guard);
        return lines;
    }
    void clear()
    {
        std::lock_guard lock(guard);
        lines.clear();
    }
};

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
    const vec4 color = fixtureBlue.load() ? vec4{0.f, 0.f, 1.f, 1.f} : vec4{1.f, 0.f, 0.f, 1.f};
    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, fixtureWidth.load(), height);
    if (fixtureAnimate.load()) {
        const auto seq = fixtureFrameSeq.fetch_add(1);
        const auto unique = fixturePacing1750.load() ? (seq / 4 * 2 + (seq % 4 != 0)) : seq;
        const vec4 bar{1.f, 1.f, 1.f, 1.f};
        gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &bar);
        gs_matrix_push();
        gs_matrix_translate3f(static_cast<float>((unique * 16) % 240), 40.f, 0.f);
        while (gs_effect_loop(effect, "Solid"))
            gs_draw_sprite(nullptr, 0, 32, 64);
        gs_matrix_pop();
    }
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
    const auto activeProps = properties(filter.get());
    auto* active = obs_properties_get(activeProps.get(), "vfg_active_status");
    expect(active && std::string_view(obs_property_description(active)).find("next open") != std::string_view::npos,
        "active preview is distinguished from selected next-open readiness");
    auto nextOpen = settings();
    obs_data_set_int(nextOpen.get(), "vfg_backend", 2);
    auto* nextBackend = obs_properties_get(activeProps.get(), "vfg_backend");
    auto* nextStatus = obs_properties_get(activeProps.get(), "vfg_status");
    expect(obs_property_modified(nextBackend, nextOpen.get()) &&
        std::string_view(obs_property_description(nextStatus)).find("requires explicit opt-in") != std::string_view::npos,
        "editing next-open backend while active updates readiness");
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()), "stop preview");
    expect(probe(input.get(), blue, true) == red, "stopping preview restores source");
    obs_queue_task(OBS_TASK_GRAPHICS, [](void*) {}, nullptr, true);
    obs_source_filter_remove(input.get(), filter.get());
}

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
};

std::string text_of(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

void properties_contract(const fs::path& data, const fs::path& scratch, const fs::path& benign)
{
    quality_contract();
    const auto locale_lookup = [&](const char* locale) {
        const auto pathUtf8 = (data / "locale" / locale).u8string();
        return std::unique_ptr<lookup_t, decltype(&text_lookup_destroy)>(
            text_lookup_create(reinterpret_cast<const char*>(pathUtf8.c_str())), text_lookup_destroy);
    };
    const auto en = locale_lookup("en-US.ini");
    const auto ko = locale_lookup("ko-KR.ini");
    expect(en && ko, "locale tables");
    const auto text = [&](const char* key) {
        const char* value{};
        expect(text_lookup_getstr(en.get(), key, &value) && value && *value, key);
        return std::string(value);
    };
    for (const bool legacy : {false, true}) {
        auto values = settings();
        if (legacy) obs_data_set_int(values.get(), "vfg_quality_mode", 1);
        auto filter = source(filterId, values.get());
        obs_source_load(filter.get());
        const auto saved = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(
            obs_source_get_settings(filter.get()), obs_data_release);
        for (const auto& [key, expected] : std::initializer_list<std::pair<const char*, int>>{
                 {"vfg_multiplier", 2}, {"vfg_quality_mode", 1}, {"vfg_fps_overlay", 1}, {"vfg_dedup_mode", 1},
                 {"vfg_backend", 0}, {"vfg_pushflow_tier", 0}, {"vfg_adaptive_target_fps", 0},
                 {"cas_strength", 35}, {"scaling_backend", 0}, {"dlss_model", 1}, {"split_compare_position", 50}})
            expect(obs_data_get_int(saved.get(), key) == expected, key);
        for (const auto* key : {"vfg_low_latency", "vfg_gpu_priority", "vfg_live_anchor"})
            expect(obs_data_get_bool(saved.get(), key), key);
        for (const auto* key : {"vfg_hide_from_capture", "vfg_hide_source_canvas", "vfg_upscale", "vfg_upscale_compare",
                 "split_compare_enabled", "vfg_experimental_anr", "vfg_hud_lock", "vfg_gpu_shot_change",
                 "vfg_pacing_repair", "vfg_adaptive_fg"})
            expect(!obs_data_get_bool(saved.get(), key), key);
        for (const auto* key : {"vfg_monitor", "dlss_runtime_path", "vfg_anr_dll_path"})
            expect(std::string_view(obs_data_get_string(saved.get(), key)).empty(), key);
    }

    auto values = settings();
    auto filter = source(filterId, values.get());
    const auto props = properties(filter.get());
    const auto get = [&](const char* key) {
        auto* property = obs_properties_get(props.get(), key);
        expect(property != nullptr, key);
        return property;
    };
    auto* group = get("vfg_grp_core");
    expect(obs_property_next(&group) && group == get("vfg_grp_ab"), "A/B group follows core");
    auto* backend = get("vfg_backend");
    auto* tier = get("vfg_pushflow_tier");
    auto* optIn = get("vfg_experimental_anr");
    auto* banner = get("vfg_anr_warning");
    auto* path = get("vfg_anr_dll_path");
    auto* adaptive = get("vfg_adaptive_fg");
    auto* target = get("vfg_adaptive_target_fps");
    auto* status = get("vfg_status");
    const auto status_has = [](obs_property_t* property, std::string_view expected) {
        const std::string_view observed = obs_property_description(property);
        std::printf("status expected=%.*s actual=%s\n", static_cast<int>(expected.size()), expected.data(), observed.data());
        expect(observed.find(expected) != std::string_view::npos, "selected backend readiness");
    };
    status_has(status, "NVIDIA VFG");
    expect(obs_property_get_type(optIn) == OBS_PROPERTY_BOOL &&
        obs_property_get_type(banner) == OBS_PROPERTY_TEXT && obs_property_text_type(banner) == OBS_TEXT_INFO &&
        !obs_property_enabled(banner), "ANR opt-in and disabled information banner");
    expect(obs_property_get_type(path) == OBS_PROPERTY_PATH && obs_property_path_type(path) == OBS_PATH_FILE &&
        std::string_view(obs_property_path_filter(path)).find("(nvngx_dlssg.dll)") != std::string_view::npos,
        "ANR B15 DLL file picker");
    const auto list = [](obs_property_t* property, std::initializer_list<int> choices) {
        expect(obs_property_get_type(property) == OBS_PROPERTY_LIST &&
            obs_property_list_format(property) == OBS_COMBO_FORMAT_INT &&
            obs_property_list_item_count(property) == choices.size(), obs_property_name(property));
        size_t index = 0;
        for (const auto choice : choices)
            expect(obs_property_list_item_int(property, index++) == choice, obs_property_name(property));
    };
    list(backend, {0, 1, 2});
    list(tier, {0, 1});
    list(target, {0, 60, 120, 144, 165, 240});
    auto* dedup = get("vfg_dedup_mode");
    list(dedup, {0, 1, 5, 6});
    expect(std::string_view(obs_property_list_item_name(dedup, 0)) == text("VFG.DedupMode.Off") &&
        std::string_view(obs_property_list_item_name(dedup, 1)) == text("VFG.DedupMode.Auto") &&
        std::string_view(obs_property_list_item_name(dedup, 2)) == text("VFG.DedupMode.Adaptive") &&
        std::string_view(obs_property_list_item_name(dedup, 3)) == text("VFG.DedupMode.Fixed"),
        "dedup choices use their locale labels");
    auto* anchor = get("vfg_live_anchor");
    expect(obs_property_get_type(anchor) == OBS_PROPERTY_BOOL &&
        std::string_view(obs_property_description(anchor)) == text("VFG.LiveAnchor"), "live anchor bool label");
    auto* qualityWarning = get("vfg_quality_warning");
    expect(obs_property_get_type(qualityWarning) == OBS_PROPERTY_TEXT &&
        obs_property_text_type(qualityWarning) == OBS_TEXT_INFO && !obs_property_enabled(qualityWarning) &&
        std::string_view(obs_property_description(qualityWarning)) == text("VFG.QualityWarning"),
        "quality warning is a read-only info hint with its locale text");
    auto* qualityChoice = get("vfg_quality_mode");
    expect(!obs_property_visible(qualityWarning), "quality warning hidden for the Balanced default");
    obs_data_set_int(values.get(), "vfg_quality_mode", 0);
    expect(obs_property_modified(qualityChoice, values.get()) && obs_property_visible(qualityWarning),
        "Fast quality shows the warning");
    obs_data_set_int(values.get(), "vfg_quality_mode", 2);
    expect(obs_property_modified(qualityChoice, values.get()) && !obs_property_visible(qualityWarning),
        "Best quality hides the warning");
    obs_data_set_int(values.get(), "vfg_quality_mode", 1);
    expect(obs_property_modified(qualityChoice, values.get()) && !obs_property_visible(qualityWarning),
        "Balanced quality hides the warning");
    std::printf("PASS VFG Veyra controls: dedup default=1 live_anchor default=true warning_gated_by_quality (label=%s)\n",
        text("VFG.QualityWarning").c_str());
    std::fflush(stdout);
    {
        const std::unique_ptr<obs_data_t, decltype(&obs_data_release)> nrDefaults(
            obs_get_source_defaults("susemi_nr_filter"), obs_data_release);
        expect(nrDefaults && !obs_data_get_bool(nrDefaults.get(), "nr_perf_model") &&
            !obs_data_get_bool(nrDefaults.get(), "nr_dedup_reuse"),
            "NR Model B and worker reuse stay opt-in by default");
        std::printf("PASS NR defaults: perf_model=%d dedup_reuse=%d (Model B opt-in)\n",
            obs_data_get_bool(nrDefaults.get(), "nr_perf_model") ? 1 : 0,
            obs_data_get_bool(nrDefaults.get(), "nr_dedup_reuse") ? 1 : 0);
        std::fflush(stdout);
    }
    {
        const auto log = scratch / "live-controls-presenter.log";
        fs::remove(log);
        wchar_t previousLog[4096]{};
        const auto previousLen = GetEnvironmentVariableW(L"OBS_NR_VFG_LOG", previousLog, ARRAYSIZE(previousLog));
        struct RestoreLog {
            std::wstring prior;
            ~RestoreLog() { SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", prior.empty() ? nullptr : prior.c_str()); }
        } restoreLog{previousLen && previousLen < ARRAYSIZE(previousLog) ?
            std::wstring(previousLog, previousLen) : std::wstring{}};
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
        TapLogCapture tapLog;
        auto input = source("vfg_test_color");
        auto liveData = settings();
        auto liveFilter = source(filterId, liveData.get());
        obs_source_filter_add(input.get(), liveFilter.get());
        obs_set_output_source(0, input.get());
        struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
        auto propsLive = properties(liveFilter.get());
        const auto offset = text_of(log).size();
        expect(obs_property_button_clicked(obs_properties_get(propsLive.get(), "vfg_off"), liveFilter.get()),
            "start 1x preview for live dedup/anchor updates");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (text_of(log).substr(offset).find("event=present_original ") == std::string::npos) {
            expect(std::chrono::steady_clock::now() < deadline, "live-control preview publishes originals");
            Sleep(1);
        }
        tapLog.clear();
        const auto wait_line = [&](const char* prefix) {
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < limit) {
                if (auto line = tapLog.last(prefix); !line.empty()) return line;
                Sleep(1);
            }
            return std::string{};
        };
        auto update = settings();
        obs_data_set_int(update.get(), "vfg_dedup_mode", 5);
        obs_data_set_bool(update.get(), "vfg_live_anchor", false);
        obs_source_update(liveFilter.get(), update.get());
        const auto applied = wait_line("[vfg] live settings applied dedup_mode=5 live_anchor=0");
        const auto saved = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(
            obs_source_get_settings(liveFilter.get()), obs_data_release);
        std::printf("live update saved_dedup=%lld saved_anchor=%d captured=%zu\n",
            static_cast<long long>(obs_data_get_int(saved.get(), "vfg_dedup_mode")),
            obs_data_get_bool(saved.get(), "vfg_live_anchor") ? 1 : 0, tapLog.snapshot().size());
        for (const auto& line : tapLog.snapshot()) std::printf("live log: %s\n", line.c_str());
        std::fflush(stdout);
        expect(!applied.empty(), "live dedup/live anchor re-apply to the open session");
        std::printf("live update: %s\n", applied.c_str());
        std::fflush(stdout);

        auto invalid = settings();
        obs_data_set_int(invalid.get(), "vfg_dedup_mode", 99);
        obs_data_set_bool(invalid.get(), "vfg_live_anchor", false);
        tapLog.clear();
        obs_source_update(liveFilter.get(), invalid.get());
        const auto clamped = wait_line("[vfg] live settings applied dedup_mode=0 live_anchor=0");
        const auto normalized = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(
            obs_source_get_settings(liveFilter.get()), obs_data_release);
        std::printf("live clamp saved_dedup=%lld captured=%zu\n",
            static_cast<long long>(obs_data_get_int(normalized.get(), "vfg_dedup_mode")), tapLog.snapshot().size());
        for (const auto& line : tapLog.snapshot()) std::printf("clamp log: %s\n", line.c_str());
        expect(!clamped.empty(), "invalid dedup mode 99 clamps to Off/Exact live");
        expect(obs_data_get_int(normalized.get(), "vfg_dedup_mode") == 0,
            "invalid dedup mode is normalized in saved settings");
        std::printf("live clamp: %s\n", clamped.c_str());
        std::fflush(stdout);
        expect(obs_property_button_clicked(obs_properties_get(propsLive.get(), "vfg_stop"), liveFilter.get()),
            "stop live-control preview");
        obs_source_filter_remove(input.get(), liveFilter.get());
    }
    const auto visibility = [&](int selected, bool allowed) {
        expect(obs_property_visible(tier) == (selected == 1) &&
            obs_property_visible(optIn) == (selected == 2) &&
            obs_property_visible(banner) == (selected == 2) &&
            obs_property_visible(path) == (selected == 2 && allowed), "backend and ANR path visibility");
    };
    visibility(0, false);
    expect(!obs_property_visible(target), "adaptive target hidden by default");
    for (const auto selected : {1, 2}) {
        obs_data_set_int(values.get(), "vfg_backend", selected);
        expect(obs_property_modified(backend, values.get()), "refresh backend properties");
        visibility(selected, false);
        status_has(status, selected == 1 ? "PushFlow Fast runtime files missing" : "Experimental ANR requires explicit opt-in");
    }
    for (const bool allowed : {true, false}) {
        obs_data_set_bool(values.get(), "vfg_experimental_anr", allowed);
        expect(obs_property_modified(optIn, values.get()), "refresh ANR opt-in");
        visibility(2, allowed);
        status_has(status, allowed ? "select nvngx_dlssg.dll" : "Experimental ANR requires explicit opt-in");
        obs_data_set_bool(values.get(), "vfg_adaptive_fg", allowed);
        expect(obs_property_modified(adaptive, values.get()) && obs_property_visible(target) == allowed,
            "adaptive target follows toggle");
    }
    obs_data_set_bool(values.get(), "vfg_experimental_anr", true);
    expect(obs_property_modified(optIn, values.get()), "enable ANR for path edits");
    auto invalid = (scratch / L"nvngx_dlssg.dll").make_preferred();
    std::ofstream(invalid) << "inert, invalid PE";
    const auto invalidUtf8 = invalid.u8string();
    obs_data_set_string(values.get(), "vfg_anr_dll_path", reinterpret_cast<const char*>(invalidUtf8.c_str()));
    expect(obs_property_modified(path, values.get()), "refresh ANR DLL path");
    status_has(status, "ANR B15 validation failed");
    const auto addon = scratch / L"inert-anr";
    const auto stagedDll = vfg_stage_anr_addon(benign, addon, true);
    const auto dll = stagedDll.u8string();
    const std::string dllSetting(dll.begin(), dll.end());
    obs_data_set_string(values.get(), "vfg_anr_dll_path", dllSetting.c_str());
    expect(obs_property_modified(path, values.get()), "refresh incomplete ANR addon");
    status_has(status, "ANR B15 companion files are missing");
    const auto lastPack = addon / L"programs/fg-lite-any-dp4a-srgb8/pack.nrx";
    std::ofstream(lastPack) << "inert validation fixture";
    expect(obs_property_modified(path, values.get()), "refresh complete ANR addon");
    status_has(status, "companion files passed validation");
    const auto pickerUtf8 = stagedDll.generic_u8string();
    const std::string pickerPath(pickerUtf8.begin(), pickerUtf8.end());
    expect(pickerPath.size() > 3 && pickerPath.substr(1, 2) == ":/" &&
        pickerPath.find('\\') == std::string::npos, "OBS picker drive path uses forward slashes");
    obs_data_set_string(values.get(), "vfg_anr_dll_path", pickerPath.c_str());
    expect(obs_property_modified(path, values.get()), "refresh picker-style ANR path");
    const std::string_view stored = obs_data_get_string(values.get(), "vfg_anr_dll_path");
    expect(stored == pickerPath, "property callback preserves the stored picker path");
    std::printf("ANR native control=%s; stored OBS picker path=%s\n", dllSetting.c_str(), stored.data());
    status_has(status, "companion files passed validation");
    const auto dotPath = pickerPath.substr(0, pickerPath.rfind('/') + 1) + "./nvngx_dlssg.dll";
    obs_data_set_string(values.get(), "vfg_anr_dll_path", dotPath.c_str());
    expect(obs_property_modified(path, values.get()), "refresh picker path with dot segment");
    status_has(status, "ANR B15 validation failed");
    obs_data_set_string(values.get(), "vfg_anr_dll_path", pickerPath.c_str());

    // Controller validates before attempting to spawn. An absent executable stops this
    // fixture at that boundary even though the complete addon passes validation.
    const auto missingPresenter = scratch / L"no-presenter-for-anr-validation.exe";
    expect(!fs::exists(missingPresenter) &&
        SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", missingPresenter.c_str()) != FALSE,
        "isolate ANR launch before spawning a presenter");
    auto launch = source(filterId, values.get());
    const auto launchProps = properties(launch.get());
    const auto savedLaunch = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(
        obs_source_get_settings(launch.get()), obs_data_release);
    expect(std::string_view(obs_data_get_string(savedLaunch.get(), "vfg_anr_dll_path")) == pickerPath,
        "launch source preserves picker spelling");
    expect(!obs_property_button_clicked(obs_properties_get(launchProps.get(), "vfg_start"), launch.get()),
        "complete benign ANR validates but cannot spawn the absent presenter");
    const auto validationLog = text_of(scratch / "canvas-presenter.log");
    expect(validationLog.starts_with("event=anr_validated sha256=") &&
        validationLog.find("event=session_started ") == std::string::npos,
        "real launch validation passes before any presenter session");
    std::printf("ANR picker launch stopped before spawn: %s", validationLog.c_str());
    expect(fs::remove(lastPack), "remove inert companion after validation");
    expect(obs_property_modified(path, values.get()), "refresh incomplete ANR addon again");
    status_has(status, "ANR B15 companion files are missing");
    obs_data_set_string(values.get(), "vfg_anr_dll_path", "");
    expect(obs_property_modified(path, values.get()), "refresh cleared ANR DLL path");
    status_has(status, "select nvngx_dlssg.dll");
    obs_data_set_int(values.get(), "vfg_backend", 1);
    expect(obs_property_modified(backend, values.get()), "return to PushFlow");
    const auto runtime = scratch / L"inert-pushflow";
    expect(SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", runtime.c_str()) != FALSE, "isolate PushFlow discovery");
    obs_data_set_int(values.get(), "vfg_pushflow_tier", 1);
    expect(obs_property_modified(tier, values.get()), "refresh Quality tier");
    status_has(status, "PushFlow Quality runtime files missing");
    fs::create_directories(runtime / L"models");
    for (const auto* file : {L"onnxruntime.dll", L"DirectML.dll", L"models/pushflow-fast.onnx"})
        std::ofstream(runtime / file) << "inert discovery fixture";
    obs_data_set_int(values.get(), "vfg_pushflow_tier", 0);
    expect(obs_property_modified(tier, values.get()), "refresh Fast tier");
    status_has(status, "PushFlow Fast runtime files found");
    obs_data_set_int(values.get(), "vfg_pushflow_tier", 1);
    auto quality = source(filterId, values.get());
    const auto qualityProps = properties(quality.get());
    status_has(obs_properties_get(qualityProps.get(), "vfg_status"), "PushFlow Quality runtime files missing");
    std::ofstream(runtime / L"models/pushflow-quality.onnx") << "inert discovery fixture";
    expect(obs_property_modified(tier, values.get()), "refresh installed Quality tier");
    status_has(status, "PushFlow Quality runtime files found");
    obs_data_set_int(values.get(), "vfg_backend", 0);
    expect(obs_property_modified(backend, values.get()), "return to Native");
    status_has(status, "NVIDIA VFG");
    obs_data_set_int(values.get(), "vfg_backend", 2);
    obs_data_set_bool(values.get(), "vfg_experimental_anr", false);
    obs_data_set_string(values.get(), "vfg_anr_dll_path", "nvngx_dlssg.dll");
    auto blocked = source(filterId, values.get());
    const auto blockedProps = properties(blocked.get());
    status_has(obs_properties_get(blockedProps.get(), "vfg_status"), "Experimental ANR requires explicit opt-in");
    expect(!obs_property_visible(obs_properties_get(blockedProps.get(), "vfg_anr_dll_path")),
        "persisted DLL path stays hidden without opt-in");
    struct RefusalLog {
        log_handler_t prior{};
        void* param{};
        bool seen{};
        ~RefusalLog() { base_set_log_handler(prior, param); }
    } refusal;
    base_get_log_handler(&refusal.prior, &refusal.param);
    base_set_log_handler([](int level, const char* format, va_list args, void* opaque) {
        auto& log = *static_cast<RefusalLog*>(opaque);
        if (std::string_view(format) == "[vfg] presenter launch refused: %s") {
            va_list copy;
            va_copy(copy, args);
            const auto* message = va_arg(copy, const char*);
            va_end(copy);
            log.seen = level == LOG_WARNING && std::string_view(message) ==
                "Experimental ANR requires explicit opt-in before opening the preview.";
        }
        log.prior(level, format, args, log.param);
    }, &refusal);
    expect(!obs_property_button_clicked(obs_properties_get(blockedProps.get(), "vfg_start"), blocked.get()) &&
        refusal.seen, "ANR start rejects opt-in OFF with explicit status");

    const auto tap = text_of(data.parent_path() / "src" / "fg" / "fg_tap.cpp");
    const std::regex localeKey(R"key("(VFG\.[A-Za-z0-9_.]+)")key");
    const std::sregex_iterator first(tap.begin(), tap.end(), localeKey), end;
    expect(first != end, "VFG locale references found");
    for (const auto* locale : {"en-US.ini", "ko-KR.ini"}) {
        const auto pathUtf8 = (data / "locale" / locale).u8string();
        const auto lookup = std::unique_ptr<lookup_t, decltype(&text_lookup_destroy)>(
            text_lookup_create(reinterpret_cast<const char*>(pathUtf8.c_str())), text_lookup_destroy);
        expect(lookup != nullptr, locale);
        for (auto it = first; it != end; ++it) {
            const auto key = (*it)[1].str();
            const char* translated{};
            if (!text_lookup_getstr(lookup.get(), key.c_str(), &translated) || !*translated)
                throw std::runtime_error(std::string(locale) + ": missing " + key);
        }
    }
    std::puts("PASS VFG defaults, callbacks, selected readiness, explicit ANR refusal and en-US/ko-KR key parity");

    const std::regex scopedKey(R"(^((?:VFG|NR)\.[A-Za-z0-9_.]+)\s*=)");
    const auto locale_keys = [&](const fs::path& path) {
        std::set<std::string> keys;
        std::ifstream stream(path);
        for (std::string line; std::getline(stream, line);) {
            std::smatch match;
            if (std::regex_search(line, match, scopedKey)) keys.insert(match[1].str());
        }
        return keys;
    };
    const auto enKeys = locale_keys(data / "locale" / "en-US.ini");
    const auto koKeys = locale_keys(data / "locale" / "ko-KR.ini");
    std::string missing;
    for (const auto& key : enKeys)
        if (!koKeys.contains(key)) missing += " ko-KR:" + key;
    for (const auto& key : koKeys)
        if (!enKeys.contains(key)) missing += " en-US:" + key;
    expect(enKeys.size() > 6 && missing.empty(), ("VFG/NR locale key parity" + missing).c_str());
    for (const auto* key : {"NR.PerfModel", "NR.Hint.PerfModel", "NR.PerfModel.Unavailable",
             "NR.DedupReuse", "NR.Hint.DedupReuse", "NR.RuntimeInfo"}) {
        for (auto* table : {en.get(), ko.get()}) {
            const char* value{};
            expect(text_lookup_getstr(table, key, &value) && value && *value, key);
        }
    }
    std::printf("PASS locale parity: %zu VFG/NR keys in en-US and ko-KR, NR Model B/Reuse/Runtime keys present\n",
        enKeys.size());
    std::fflush(stdout);
}

template<class Predicate>
void await(Predicate test, const char* description, int timeoutMs = 6000)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!test()) {
        expect(Clock::now() < deadline, description);
        Sleep(1);
    }
}

std::string field(const std::string& line, const char* key)
{
    const auto pos = line.find(key);
    if (pos == std::string::npos) return {};
    const auto start = pos + strlen(key);
    return line.substr(start, line.find_first_of(" \r\n", start) - start);
}

std::string event(const std::string& text, const char* token, const char* epoch = "")
{
    for (size_t start = 0; start < text.size();) {
        const auto end = text.find('\n', start);
        if (end == std::string::npos) break;
        const auto line = text.substr(start, end - start);
        if (line.starts_with(token) && line.find(epoch) != std::string::npos) return line;
        start = end + 1;
    }
    return {};
}

size_t count_events(const std::string& text, const char* token)
{
    size_t count = 0;
    for (auto pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + 1)) ++count;
    return count;
}

struct TestWaitSpan {
    volatile int64_t waitStartNs;
    volatile int64_t waitEndNs;
};

int64_t qpc_now_ns() noexcept
{
    LARGE_INTEGER tick{}, rate{};
    QueryPerformanceCounter(&tick);
    QueryPerformanceFrequency(&rate);
    const auto t = static_cast<uint64_t>(tick.QuadPart), f = static_cast<uint64_t>(rate.QuadPart);
    return static_cast<int64_t>(t / f * 1000000000ULL + t % f * 1000000000ULL / f);
}

struct RenderStats {
    std::atomic_uint64_t rawCount{}, gaps{}, maxNs{}, ticks{};
    const TestWaitSpan* waitSpan{};
    uint64_t lastTimestamp{};
    int64_t tickStartedNs{};
    static void tick(void* opaque, float)
    {
        static_cast<RenderStats*>(opaque)->tickStartedNs = qpc_now_ns();
    }
    static void rendered(void* opaque)
    {
        auto& s = *static_cast<RenderStats*>(opaque);
        const int64_t tickStart = s.tickStartedNs;
        if (!tickStart) return;
        const int64_t now = qpc_now_ns();
        if (now <= tickStart) return;
        int64_t elapsed = now - tickStart;
        if (s.waitSpan) {
            const int64_t wStart = InterlockedCompareExchange64(const_cast<volatile int64_t*>(&s.waitSpan->waitStartNs), 0, 0);
            const int64_t wEnd = InterlockedCompareExchange64(const_cast<volatile int64_t*>(&s.waitSpan->waitEndNs), 0, 0);
            if (wEnd > wStart && wEnd > tickStart && wStart < now) {
                const int64_t overlapStart = wStart > tickStart ? wStart : tickStart;
                const int64_t overlapEnd = wEnd < now ? wEnd : now;
                if (overlapEnd > overlapStart) elapsed -= (overlapEnd - overlapStart);
            }
        }
        const auto ns = static_cast<uint64_t>(elapsed);
        auto prior = s.maxNs.load();
        while (ns > prior && !s.maxNs.compare_exchange_weak(prior, ns)) {}
        ++s.ticks;
    }
    static void raw(void* opaque, video_data* frame)
    {
        auto& s = *static_cast<RenderStats*>(opaque);
        if (s.lastTimestamp && frame->timestamp - s.lastTimestamp > 25'000'000) ++s.gaps;
        s.lastTimestamp = frame->timestamp;
        ++s.rawCount;
    }
    RenderStats()
    {
        obs_add_tick_callback(tick, this);
        obs_add_main_rendered_callback(rendered, this);
        obs_add_raw_video_callback(nullptr, raw, this);
    }
    ~RenderStats()
    {
        obs_remove_raw_video_callback(raw, this);
        obs_remove_main_rendered_callback(rendered, this);
        obs_remove_tick_callback(tick, this);
    }
};

enum class Action { Escape, Close, ExitZero, Fault, Reject, ResizeFault, Kill, Watchdog, Stop, Detach, BadPeer };
struct Scenario {
    const wchar_t* name;
    Action action;
    const wchar_t* stall{};
    const wchar_t* rejectedStall{};
    const wchar_t* checkpoint{};
    bool timing{};
    bool closeRecovered{};
};

void recovery_case(const fs::path& scratch, const Scenario& s)
{
    const auto log = scratch / (std::wstring(s.name) + L".log");
    fs::remove(log);
    const auto marks = scratch / L"obs-nr-native-filter";
    fs::remove_all(marks);
    const auto quarantine = marks / L"pushflow-crash-quarantine.txt";
    const auto key = LR"(Local\nr-t2-)" + std::to_wstring(GetCurrentProcessId()) + L"-" + s.name;
    const Handle reached{CreateEventW(nullptr, TRUE, FALSE, key.c_str())},
        resume{CreateEventW(nullptr, TRUE, FALSE, (key + L".resume").c_str())},
        armFault{CreateEventW(nullptr, TRUE, FALSE, (key + L".fault").c_str())},
        waitMap{CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(TestWaitSpan),
            (key + L".wait").c_str())};
    expect(reached.value && resume.value && armFault.value && waitMap.value, "create isolated checkpoint events");
    auto* waitView = static_cast<TestWaitSpan*>(MapViewOfFile(waitMap.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestWaitSpan)));
    expect(waitView != nullptr, "map isolated checkpoint wait span");
    struct WaitViewGuard {
        TestWaitSpan* ptr;
        ~WaitViewGuard() { if (ptr) UnmapViewOfFile(ptr); }
    } waitViewGuard{waitView};
    *waitView = {};
    const bool resize = s.action == Action::ResizeFault;
    const bool recover = s.action != Action::Escape && s.action != Action::Close &&
        s.action != Action::Stop && s.action != Action::Detach && s.action != Action::BadPeer;
    const bool earlyCheckpoint = s.checkpoint && (wcscmp(s.checkpoint, L"before_alive") == 0 ||
        wcscmp(s.checkpoint, L"before_connect") == 0 || wcscmp(s.checkpoint, L"bad_peer") == 0);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_WORKER_MS", s.stall);
    SetEnvironmentVariableW(L"OBS_NR_TEST_REJECT_FIRST_IMPORT", s.action == Action::Reject ? L"1" : nullptr);
    SetEnvironmentVariableW(L"OBS_NR_TEST_REJECT_STALL_MS", s.rejectedStall);
    SetEnvironmentVariableW(L"OBS_NR_TEST_FAULT_EXIT_DELAY_MS", s.action == Action::Fault || resize ? L"700" : nullptr);
    SetEnvironmentVariableW(L"OBS_NR_TEST_FAULT_ARM_EVENT", resize ? (key + L".fault").c_str() : nullptr);
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", s.closeRecovered ? L"2500" : nullptr);
    SetEnvironmentVariableW(L"OBS_NR_TEST_TAP_EVENT", key.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_TAP_STAGE", earlyCheckpoint ? s.checkpoint : nullptr);
    fixtureWidth.store(width);
    auto input = source("vfg_test_color");
    auto data = settings();
    obs_data_set_int(data.get(), "vfg_backend", 1);
    auto filter = source(filterId, data.get());
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    const auto props = properties(filter.get());
    RenderStats stats;
    stats.waitSpan = waitView;
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get()), "start PushFlow");
    std::string initial;
    await([&] {
        initial = event(text_of(log), "event=session_started backend=pushflow ", "epoch=1");
        return !initial.empty();
    }, "confirm PushFlow before injection");
    const auto oldPid = std::stoul(field(initial, "pid="));
    const Handle old{OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, oldPid)};
    expect(old.value != nullptr, "open owned original presenter");
    std::string oldOffer;
    if (!earlyCheckpoint) await([&] {
        oldOffer = event(text_of(log), "event=offer ", "epoch=1");
        return !oldOffer.empty();
    }, "original IPC offer");
    const auto hwnd = [&] {
        return reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(field(oldOffer, "hwnd="), nullptr, 16)));
    };
    const auto started = Clock::now();
    const auto beginRaw = stats.rawCount.load();
    const auto beginGaps = stats.gaps.load();
    switch (s.action) {
    case Action::Escape:
    case Action::Close: {
        const auto ready = s.stall ? "event=test_worker_stalled" : "event=present_original ";
        await([&] { return !event(text_of(log), ready).empty(); }, "presenter ready for user close");
        if (!s.stall) {
            expect(!event(text_of(log), "event=pushflow_runtime_missing_fallback_1x").empty() &&
                count_events(text_of(log), "event=present_vfg_subframe ") == 0,
                "empty PushFlow runtime outputs only originals");
            for (const auto enabled : {true, false}) {
                const auto offset = text_of(log).size();
                auto live = settings();
                obs_data_set_int(live.get(), "vfg_backend", 1);
                obs_data_set_bool(live.get(), "vfg_hud_lock", enabled);
                obs_data_set_bool(live.get(), "vfg_pacing_repair", enabled);
                obs_data_set_bool(live.get(), "vfg_adaptive_fg", enabled);
                obs_data_set_int(live.get(), "vfg_adaptive_target_fps", enabled ? 120 : 0);
                obs_data_set_bool(live.get(), "vfg_gpu_shot_change", enabled);
                obs_data_set_int(live.get(), "vfg_pushflow_tier", enabled ? 1 : 0);
                obs_source_update(filter.get(), live.get());
                fixtureBlue.store(enabled);
                const auto wanted = enabled ? "hud_lock=1 shot_cut=0 pacing_repair=1 adaptive_fg=1 adaptive_target_fps=120" :
                    "hud_lock=0 shot_cut=0 pacing_repair=0 adaptive_fg=0 adaptive_target_fps=0";
                await([&] { return !event(text_of(log).substr(offset), "event=present_original ", wanted).empty(); },
                    "live settings reach the presenter without relaunch");
            }
        }
        const auto message = s.action == Action::Escape ? WM_KEYDOWN : WM_CLOSE;
        const auto closeSent = Clock::now();
        expect(PostMessageW(hwnd(), message, message == WM_KEYDOWN ? VK_ESCAPE : 0, 0) != FALSE, "user close");
        if (s.stall) {
            await([&] { return WaitForSingleObject(old.value, 0) == WAIT_OBJECT_0; }, "bounded stalled user close", 1800);
            const auto closeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - closeSent).count();
            expect(closeElapsed < 1800, "stalled user close exited within 1800 ms of message");
            std::printf("user_close message=%s send_to_exit_ms=%lld\n", message == WM_CLOSE ? "WM_CLOSE" : "Esc",
                static_cast<long long>(closeElapsed));
        }
        break;
    }
    case Action::ResizeFault:
        await([&] { return !event(text_of(log), "event=present_original ").empty(); }, "active connection before resize");
        SetEnvironmentVariableW(L"OBS_NR_TEST_TAP_STAGE", L"before_extent");
        expect(WaitForSingleObject(reached.value, 2000) == WAIT_OBJECT_0, "pause before extent");
        SetEvent(armFault.value);
        await([&] { return text_of(log).find("test_live_consumer_fault") != std::string::npos; },
            "fault state before resize", 225);
        fixtureWidth.store(352);
        SetEvent(resume.value);
        break;
    case Action::Kill:
    case Action::ExitZero:
        if (s.checkpoint) {
            if (!earlyCheckpoint) {
                await([&] { return !event(text_of(log), "event=present_original ").empty(); }, "active connection before kill");
                SetEnvironmentVariableW(L"OBS_NR_TEST_TAP_STAGE", s.checkpoint);
            }
            expect(WaitForSingleObject(reached.value, 2000) == WAIT_OBJECT_0, "kill checkpoint reached");
        }
        expect(TerminateProcess(old.value, s.action == Action::ExitZero ? 0 : 7) != FALSE &&
            WaitForSingleObject(old.value, s.checkpoint ? 200 : 1500) == WAIT_OBJECT_0, "terminate owned presenter");
        SetEvent(resume.value);
        break;
    case Action::Stop:
        await([&] { return !event(text_of(log), "event=present_original ").empty(); }, "active connection before stop");
        expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()), "parent stop");
        break;
    case Action::Detach:
        await([&] { return !event(text_of(log), "event=present_original ").empty(); }, "active connection before detach");
        obs_source_filter_remove(input.get(), filter.get());
        break;
    case Action::BadPeer:
        expect(WaitForSingleObject(reached.value, 2000) == WAIT_OBJECT_0 &&
            WaitForSingleObject(old.value, 0) == WAIT_TIMEOUT, "corrupt identity while original child lives");
        SetEvent(resume.value);
        break;
    case Action::Fault:
    case Action::Reject:
    case Action::Watchdog: break;
    }
    await([&] { return WaitForSingleObject(old.value, 0) == WAIT_OBJECT_0; }, "bounded original child shutdown");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    DWORD oldExit{};
    expect(GetExitCodeProcess(old.value, &oldExit) != FALSE, "owned original exit code");
    std::string newOffer, transcript;
    if (recover) {
        await([&] {
            transcript = text_of(log);
            newOffer = event(transcript, "event=offer ", "epoch=2");
            if (newOffer.empty()) return false;
            const auto generation = "generation=" + field(newOffer, "generation=") + " ";
            return !event(transcript, "event=present_original ", generation.c_str()).empty();
        }, "new 1x PID resumes original preview");
        expect(std::stoul(field(newOffer, "pid=")) != oldPid && field(newOffer, "mode=") == "1x" &&
            (!oldOffer.size() || field(newOffer, "generation=") != field(oldOffer, "generation=")), "fresh PID/generation in 1x mode");
        expect(!resize || field(newOffer, "size=") == "352x180", "recovered at the resized extent");
        expect(text_of(quarantine) == "reason=controller_unexpected_exit\n", "parent persisted crash quarantine");
        if (s.action == Action::Watchdog)
            expect(oldExit == 2 && !event(transcript, "event=watchdog_timeout ").empty() &&
                !event(transcript, "event=shutdown_force_terminate watchdog=1 exit_reason=3").empty(), "watchdog chose forced exit");
        if (s.timing)
            expect(stats.rawCount.load() > beginRaw && stats.gaps.load() == beginGaps &&
                stats.ticks.load() > 0 && stats.maxNs.load() < 5'000'000, "render under 5 ms; continuous raw broadcast");
        const auto newPid = std::stoul(field(newOffer, "pid="));
        const Handle child{OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, newPid)};
        expect(child.value != nullptr, "open owned replacement");
        if (s.closeRecovered) {
            const auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(field(newOffer, "hwnd="), nullptr, 16)));
            expect(PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0) != FALSE, "Esc on recovered 1x");
        } else expect(TerminateProcess(child.value, 8) != FALSE, "second unexpected exit");
        await([&] { return WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0; }, "replacement closed");
        if (s.closeRecovered) {
            await([&] {
                return !event(text_of(log), "event=shutdown_force_terminate watchdog=0 exit_reason=1").empty();
            }, "forced Esc in recovered 1x diagnostic", 1000);
            expect(text_of(quarantine) == "reason=controller_unexpected_exit\n",
                "forced Esc in recovered 1x preserves quarantine");
        }
    } else {
        if (oldExit != 0)
            throw std::runtime_error("user/parent close exited " + std::to_string(oldExit));
        if (s.stall || (s.action == Action::Stop && s.closeRecovered))
            await([&] {
                return !event(text_of(log), "event=shutdown_force_terminate watchdog=0 exit_reason=1").empty();
            }, "stalled user close preserves UserClosed", 1000);
        expect(!fs::exists(quarantine), "intentional stop never quarantined");
    }
    const auto observed = stats.rawCount.load();
    await([&] { return stats.rawCount.load() >= observed + 10; }, "broadcast continues after final child exit");
    transcript = text_of(log);
    const auto restarts = count_events(transcript, "event=controller_auto_fallback_1x_relaunch");
    expect(restarts == static_cast<size_t>(recover), "exact relaunch count after the last close");
    const auto oldGenStr = oldOffer.empty() ? std::string("none(pre-offer)") : field(oldOffer, "generation=");
    if (recover) {
        expect(!field(newOffer, "generation=").empty(), "recovered offer has generation");
        if (earlyCheckpoint)
            expect(event(transcript, "event=offer ", "epoch=1").empty(),
                "early checkpoint transcript must have no epoch=1 offer");
    }
    std::printf("PASS %ls old_pid=%lu old_exit=%lu old_generation=%s new_pid=%s new_generation=%s relaunches=%zu "
        "elapsed_ms=%lld max_render_ns=%llu raw_frames=%llu raw_gaps=%llu\n", s.name, oldPid, oldExit,
        oldGenStr.c_str(), field(newOffer, "pid=").c_str(), field(newOffer, "generation=").c_str(),
        restarts, static_cast<long long>(elapsed), static_cast<unsigned long long>(stats.maxNs.load()),
        static_cast<unsigned long long>(stats.rawCount.load()), static_cast<unsigned long long>(stats.gaps.load()));
    if (s.action != Action::Detach) obs_source_filter_remove(input.get(), filter.get());
}

constexpr std::array scenarios{
        Scenario{L"a-esc", Action::Escape},
        Scenario{L"wm-close", Action::Close},
        Scenario{L"b-esc-stalled", Action::Escape, L"7000"},
        Scenario{L"wm-close-stalled", Action::Close, L"7000"},
        Scenario{L"c-exit-zero", Action::ExitZero, nullptr, nullptr, nullptr, true, true},
        Scenario{L"d-live-consumer-fault", Action::Fault, nullptr, nullptr, nullptr, true},
        Scenario{L"e-import-rejected", Action::Reject, nullptr, nullptr, nullptr, true},
        Scenario{L"e-import-rejected-stalled", Action::Reject, nullptr, L"700", nullptr, true},
        Scenario{L"f-resize-fault", Action::ResizeFault, nullptr, nullptr, nullptr, true},
        Scenario{L"g-before-publish", Action::Kill, nullptr, nullptr, L"before_alive", true},
        Scenario{L"h-before-connect", Action::Kill, nullptr, nullptr, L"before_connect", true},
        Scenario{L"i-after-alive", Action::Kill, nullptr, nullptr, L"after_alive", true},
        Scenario{L"j-watchdog", Action::Watchdog, L"7000", nullptr, nullptr, true},
        Scenario{L"parent-stop", Action::Stop},
        Scenario{L"parent-stop-stalled", Action::Stop, nullptr, nullptr, nullptr, false, true},
        Scenario{L"parent-detach", Action::Detach},
        Scenario{L"bad-live-peer", Action::BadPeer, nullptr, nullptr, L"bad_peer"},
};

void immediate_esc_reopen_case(const fs::path& scratch, const fs::path& log, const std::wstring& pfRuntime)
{
    const auto marks = scratch / L"obs-nr-native-filter";
    fs::remove_all(marks);
    SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);
    fixtureAnimate.store(true);
    struct ResetAnimate { ~ResetAnimate() { fixtureAnimate.store(false); } } resetAnimate;

    auto input = source("vfg_test_color");
    auto data = settings();
    obs_data_set_int(data.get(), "vfg_backend", 1);
    auto filter = source(filterId, data.get());
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    const auto props = properties(filter.get());

    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get()), "start PushFlow");
    std::string firstInit;
    await([&] {
        const auto t = text_of(log);
        firstInit = event(t, "event=initialized backend=pushflow ");
        return !firstInit.empty() && count_events(t, "event=present_vfg_subframe ") >= 3 &&
            !event(t, "event=offer ", "epoch=1").empty();
    }, "initial PushFlow synthesis before immediate Esc");
    const auto firstOffer = event(text_of(log), "event=offer ", "epoch=1");
    const auto firstPid = std::stoul(field(firstOffer, "pid="));
    const auto hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(field(firstOffer, "hwnd="), nullptr, 16)));
    const Handle firstProc{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, firstPid)};
    expect(firstProc.value != nullptr, "open first PushFlow presenter");
    expect([&] {
        std::error_code ec;
        for (fs::directory_iterator it(marks, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().wstring();
            if (name == L"pushflow-starting.txt" ||
                (name.starts_with(L"pushflow-starting-") && name.ends_with(L".txt"))) return true;
        }
        return false;
    }(), "pushflow starting marker armed during synthesis");

    expect(PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0) != FALSE, "Esc on PushFlow preview");
    // Wait ONLY for OS child exit (no test-side await_exit_cleanup) to test the immediate OBS reopen boundary.
    await([&] { return WaitForSingleObject(firstProc.value, 0) == WAIT_OBJECT_0; }, "bounded Esc exit", 2500);

    const auto reopenOffset = text_of(log).size();
    await([&] {
        return obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get());
    }, "immediate libobs vfg_start right after Esc", 2000);
    await([&] {
        const auto tail = text_of(log).substr(reopenOffset);
        return !event(tail, "event=initialized backend=pushflow ").empty() &&
            count_events(tail, "event=present_vfg_subframe ") >= 3;
    }, "immediate reopen resumes PushFlow synthesis", 10000);

    const auto tail = text_of(log).substr(reopenOffset);
    expect(tail.find("event=pushflow_previous_crash_detected") == std::string::npos,
        "immediate OBS reopen after Esc must NOT misclassify as previous crash");
    const auto secondOffer = event(tail, "event=offer ", "epoch=1");
    expect(std::stoul(field(secondOffer, "pid=")) != firstPid && field(secondOffer, "mode=") == "2x",
        "immediate reopen launched fresh 2x PushFlow PID");
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()), "stop reopened PushFlow");
    obs_source_filter_remove(input.get(), filter.get());
    std::printf("PASS k-immediate-esc-reopen first_pid=%lu second_pid=%s subframes=%zu\n",
        firstPid, field(secondOffer, "pid=").c_str(), count_events(tail, "event=present_vfg_subframe "));
}

void immediate_esc_reopen_stalled_diag_case(const fs::path& scratch, const fs::path& log, const std::wstring& pfRuntime)
{
    const auto marks = scratch / L"obs-nr-native-filter";
    fs::remove_all(marks);
    fs::remove(log);
    wchar_t prevLog[4096]{};
    const auto prevLogLen = GetEnvironmentVariableW(L"OBS_NR_VFG_LOG", prevLog, ARRAYSIZE(prevLog));
    SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());

    const auto stallName = LR"(Local\obs-nr-libobs-diag-stall-)" + std::to_wstring(GetCurrentProcessId());
    const auto reapName = LR"(Local\obs-nr-libobs-diag-reap-)" + std::to_wstring(GetCurrentProcessId());
    const Handle stallEv{CreateEventW(nullptr, TRUE, FALSE, stallName.c_str())};
    const Handle reapEv{CreateEventW(nullptr, TRUE, FALSE, reapName.c_str())};
    expect(stallEv.value && reapEv.value, "create libobs diag stall events");

    fixtureAnimate.store(true);
    struct DiagStallGuard {
        HANDLE ev{};
        std::wstring restoreLog;
        ~DiagStallGuard()
        {
            if (ev) SetEvent(ev);
            SetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_STALL_EVENT", nullptr);
            SetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_REAPED_EVENT", nullptr);
            SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);
            SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", restoreLog.empty() ? nullptr : restoreLog.c_str());
            fixtureAnimate.store(false);
        }
    } guard{stallEv.value, prevLogLen && prevLogLen < ARRAYSIZE(prevLog) ? std::wstring(prevLog, prevLogLen) : std::wstring{}};

    SetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_STALL_EVENT", stallName.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_DIAG_REAPED_EVENT", reapName.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);

    auto input = source("vfg_test_color");
    auto data = settings();
    obs_data_set_int(data.get(), "vfg_backend", 1);
    auto filter = source(filterId, data.get());
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    const auto props = properties(filter.get());

    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get()), "start PushFlow for stalled diag");
    await([&] {
        const auto t = text_of(log);
        return !event(t, "event=initialized backend=pushflow ").empty() &&
            count_events(t, "event=present_vfg_subframe ") >= 3 &&
            !event(t, "event=offer ", "epoch=1").empty();
    }, "initial PushFlow synthesis before diag-stalled Esc");
    const auto firstOffer = event(text_of(log), "event=offer ", "epoch=1");
    const auto firstPid = std::stoul(field(firstOffer, "pid="));
    const auto hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(field(firstOffer, "hwnd="), nullptr, 16)));
    const Handle firstProc{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, firstPid)};
    expect(firstProc.value != nullptr, "open first PushFlow presenter (diag-stalled)");

    // Lock the armed starting marker without FILE_SHARE_DELETE so observer cleanup fails cleanly,
    // writes pushflow-user-closed.txt, and triggers write_event_bounded() -> TP_WORK stall on stallEv.
    fs::path armedMarker;
    await([&] {
        std::error_code ec;
        for (fs::directory_iterator it(marks, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().wstring();
            if (name == L"pushflow-starting.txt" ||
                (name.starts_with(L"pushflow-starting-") && name.ends_with(L".txt"))) {
                armedMarker = it->path();
                return true;
            }
        }
        return false;
    }, "locate armed starting marker before diag-stalled Esc");
    {
        Handle markerLock{CreateFileW(armedMarker.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        expect(markerLock.value != INVALID_HANDLE_VALUE, "lock armed starting marker without FILE_SHARE_DELETE");

        expect(PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0) != FALSE, "Esc on PushFlow preview (diag-stalled)");
        await([&] { return WaitForSingleObject(firstProc.value, 0) == WAIT_OBJECT_0; }, "bounded Esc exit", 2500);
        // Allow observer thread to record fallback, enter write_event_bounded() -> TP_WORK, and block on stallEv.
        await([&] { return fs::exists(marks / L"pushflow-user-closed.txt"); },
            "observer writes user-closed fallback before diag TP_WORK", 2000);
        Sleep(350);
    }
    expect(WaitForSingleObject(reapEv.value, 20) == WAIT_TIMEOUT,
        "diagnostic TP_WORK must still be stalled before immediate libobs reopen");

    // Reopen while diagnostic TP_WORK from session 1 is STILL blocked on stallEv.
    const auto reopenOffset = text_of(log).size();
    await([&] {
        return obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get());
    }, "immediate libobs vfg_start while prior diag TP_WORK is stalled", 2000);
    await([&] {
        const auto tail = text_of(log).substr(reopenOffset);
        return !event(tail, "event=initialized backend=pushflow ").empty() &&
            count_events(tail, "event=present_vfg_subframe ") >= 3;
    }, "reopen resumes 2x PushFlow synthesis while diag TP_WORK remains stalled", 10000);
    expect(WaitForSingleObject(reapEv.value, 20) == WAIT_TIMEOUT,
        "prior diagnostic TP_WORK remained blocked throughout reopen synthesis");

    const auto tail = text_of(log).substr(reopenOffset);
    expect(tail.find("event=pushflow_previous_crash_detected") == std::string::npos,
        "stalled-diag libobs reopen must NOT misclassify as previous crash");
    const auto secondOffer = event(tail, "event=offer ", "epoch=1");
    expect(std::stoul(field(secondOffer, "pid=")) != firstPid && field(secondOffer, "mode=") == "2x",
        "stalled-diag libobs reopen launched fresh 2x PushFlow PID");
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()), "stop reopened PushFlow (diag-stalled)");
    obs_source_filter_remove(input.get(), filter.get());

    // Release stalled diagnostic TP_WORK and verify deferred write completes cleanly.
    SetEvent(stallEv.value);
    expect(WaitForSingleObject(reapEv.value, 2000) == WAIT_OBJECT_0,
        "stalled diagnostic TP_WORK finishes and releases resources once stall clears");
    const auto fullLog = text_of(log);
    expect(fullLog.find("event=pushflow_marker_cleanup_failed") != std::string::npos,
        "deferred diag write completed cleanly to log after stall cleared");
    std::printf("PASS k-immediate-esc-reopen-diag-stalled first_pid=%lu second_pid=%s subframes=%zu\n",
        firstPid, field(secondOffer, "pid=").c_str(), count_events(tail, "event=present_vfg_subframe "));
}

void mutex_failure_fallback_unlocks_tap_case(const fs::path& scratch, const fs::path& log, const std::wstring& pfRuntime)
{
    const auto marks = scratch / L"obs-nr-native-filter";
    fs::remove_all(marks);
    fs::remove(log);
    wchar_t prevLog[4096]{};
    const auto prevLogLen = GetEnvironmentVariableW(L"OBS_NR_VFG_LOG", prevLog, ARRAYSIZE(prevLog));
    SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);
    SetEnvironmentVariableW(L"OBS_NR_TEST_MARKER_MUTEX_FAIL", L"1");

    fixtureAnimate.store(true);
    struct MutexFailGuard {
        std::wstring restoreLog;
        ~MutexFailGuard()
        {
            SetEnvironmentVariableW(L"OBS_NR_TEST_MARKER_MUTEX_FAIL", nullptr);
            SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", restoreLog.empty() ? nullptr : restoreLog.c_str());
            fixtureAnimate.store(false);
        }
    } guard{prevLogLen && prevLogLen < ARRAYSIZE(prevLog) ? std::wstring(prevLog, prevLogLen) : std::wstring{}};

    auto input = source("vfg_test_color");
    auto data = settings();
    obs_data_set_int(data.get(), "vfg_backend", 1);
    auto filter = source(filterId, data.get());
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    const auto props = properties(filter.get());

    const auto tStart0 = qpc_now_ns();
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get()),
        "libobs vfg_start under CreateMutexW failure returns cleanly");
    const auto startMs = (qpc_now_ns() - tStart0) / 1'000'000LL;
    expect(startMs < 1000, "libobs vfg_start under CreateMutexW failure must NOT spin on tap.command");

    await([&] {
        const auto t = text_of(log);
        return t.find("event=pushflow_marker_lock_failed_fallback_1x") != std::string::npos &&
            count_events(t, "event=present_original ") >= 3;
    }, "1x fallback output under CreateMutexW failure", 6000);
    const auto t1 = text_of(log);
    expect(count_events(t1, "event=present_vfg_subframe ") == 0 &&
        t1.find("mode=1x") != std::string::npos && t1.find("mode=2x") == std::string::npos,
        "CreateMutexW failure must NOT start unprotected 2x PushFlow");

    // Verify tap.command lock was released cleanly by stopping and then reopening normal 2x PushFlow.
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()),
        "libobs vfg_stop after CreateMutexW fallback proves tap.command lock released");
    SetEnvironmentVariableW(L"OBS_NR_TEST_MARKER_MUTEX_FAIL", nullptr);
    const auto reopenOff = text_of(log).size();
    await([&] {
        return obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get());
    }, "libobs vfg_start resumes 2x PushFlow after mutex failure clears", 2500);
    await([&] {
        const auto tail = text_of(log).substr(reopenOff);
        return !event(tail, "event=initialized backend=pushflow ").empty() &&
            count_events(tail, "event=present_vfg_subframe ") >= 3;
    }, "2x PushFlow synthesis resumes after mutex fallback recovery", 10000);
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()), "stop resumed 2x PushFlow");
    obs_source_filter_remove(input.get(), filter.get());
    std::printf("PASS k-mutex-fail-fallback-unlock start_ms=%lld orig=%zu\n",
        static_cast<long long>(startMs), count_events(t1, "event=present_original "));
}

void pacing_repair_ab_case(const fs::path& log)
{
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);
    fixtureFrameSeq.store(0);
    fixturePacing1750.store(true);
    fixtureAnimate.store(true);
    struct ResetFixture {
        ~ResetFixture()
        {
            fixtureAnimate.store(false);
            fixturePacing1750.store(false);
        }
    } resetFixture;

    auto input = source("vfg_test_color");
    auto data = settings();
    obs_data_set_int(data.get(), "vfg_backend", 0);
    obs_data_set_int(data.get(), "vfg_multiplier", 2);
    obs_data_set_bool(data.get(), "vfg_pacing_repair", false);
    obs_data_set_bool(data.get(), "vfg_adaptive_fg", false);
    auto filter = source(filterId, data.get());
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    const auto props = properties(filter.get());

    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get()),
        "start NativeVfg for real-OBS pacing repair A/B");
    await([&] {
        return !event(text_of(log), "event=initialized backend=nvidia-vfg-native ", "pacing_repair=0 adaptive_fg=0").empty();
    }, "NativeVfg initialized with pacing_repair=0 adaptive_fg=0", 12000);

    struct OrigSample { uint64_t serial; double qpcMs; int repair; int64_t shiftNs; };
    const auto parseOriginals = [](const std::string& text) {
        std::vector<OrigSample> out;
        std::istringstream iss(text);
        for (std::string line; std::getline(iss, line);) {
            if (!line.starts_with("event=present_original ")) continue;
            const auto b = field(line, " b=");
            const auto q = field(line, " qpc_ns=");
            const auto pr = field(line, " pacing_repair=");
            const auto sh = field(line, " pacing_shift_ns=");
            if (b.empty() || q.empty() || pr.empty() || sh.empty()) continue;
            out.push_back({std::stoull(b), static_cast<double>(std::stoull(q)) / 1e6, std::stoi(pr), std::stoll(sh)});
        }
        return out;
    };

    const auto countWhere = [&](int wantRepair, size_t startIdx) {
        const auto rows = parseOriginals(text_of(log));
        size_t count = 0;
        for (size_t i = startIdx; i < rows.size(); ++i)
            if (rows[i].repair == wantRepair) ++count;
        return count;
    };

    await([&] { return countWhere(0, 0) >= 56; }, "segment 1 (OFF) 56 originals", 10000);
    const size_t seg1EndIdx = parseOriginals(text_of(log)).size();

    {
        auto up = settings();
        obs_data_set_bool(up.get(), "vfg_pacing_repair", true);
        obs_source_update(filter.get(), up.get());
    }
    await([&] { return countWhere(1, seg1EndIdx) >= 56; }, "segment 2 (ON) 56 originals", 10000);
    const size_t seg2EndIdx = parseOriginals(text_of(log)).size();

    {
        auto up = settings();
        obs_data_set_bool(up.get(), "vfg_pacing_repair", false);
        obs_source_update(filter.get(), up.get());
    }
    await([&] { return countWhere(0, seg2EndIdx) >= 56; }, "segment 3 (OFF) 56 originals", 10000);

    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()),
        "stop NativeVfg after real-OBS pacing repair A/B");
    obs_source_filter_remove(input.get(), filter.get());

    const auto fullLog = text_of(log);
    wchar_t copyDest[4096]{};
    const auto copyLen = GetEnvironmentVariableW(L"OBS_NR_VFG_PACING_LOG", copyDest, ARRAYSIZE(copyDest));
    if (copyLen && copyLen < ARRAYSIZE(copyDest)) {
        std::error_code ec;
        fs::create_directories(fs::path(copyDest).parent_path(), ec);
        fs::copy_file(log, fs::path(copyDest), fs::copy_options::overwrite_existing, ec);
        expect(!ec, "copy real-OBS pacing repair presenter log");
    }

    expect(count_events(fullLog, "event=offer ") == 1, "single presenter PID across OFF->ON->OFF");
    expect(count_events(fullLog, "adaptive_fg=1") == 0, "adaptive_fg must stay 0");
    expect(count_events(fullLog, "hud_lock=1") == 0, "hud_lock must stay 0");
    expect(count_events(fullLog, "event=error") == 0, "zero errors in real-OBS pacing repair A/B");
    expect(fullLog.find("event=cadence_locked period_ns=33333333 repairing=1") != std::string::npos,
        "UniqueCadence 30 Hz lock observed during ON segment");

    const auto rows = parseOriginals(fullLog);
    const auto checkSegment = [&](int segNum, int wantRepair, size_t lo, size_t hi) {
        std::vector<OrigSample> seg;
        for (size_t i = lo; i < hi && i < rows.size(); ++i)
            if (rows[i].repair == wantRepair) seg.push_back(rows[i]);
        expect(seg.size() >= 56, "insufficient segment originals");
        std::vector<double> errs;
        int64_t shiftNz = 0;
        for (size_t i = 17; i < seg.size(); ++i) {
            errs.push_back(std::abs((seg[i].qpcMs - seg[i - 1].qpcMs) - 33.333333));
            if (seg[i].shiftNs != 0) ++shiftNz;
        }
        for (size_t i = 0; i < 17; ++i)
            if (seg[i].shiftNs != 0) ++shiftNz;
        std::sort(errs.begin(), errs.end());
        const size_t p90Idx = std::min(errs.size() - 1, (errs.size() * 9 + 9) / 10 - 1);
        const double p90 = errs[p90Idx];
        const double maxErr = errs.back();
        size_t subs = 0;
        std::istringstream iss(fullLog);
        for (std::string line; std::getline(iss, line);) {
            if (!line.starts_with("event=present_vfg_subframe ")) continue;
            const auto b = field(line, "b=");
            if (b.empty()) continue;
            const auto s = std::stoull(b);
            if (s >= seg.front().serial && s <= seg.back().serial) ++subs;
        }
        std::printf("PACING_LIBOBS seg=%d repair=%d first_serial=%llu last_serial=%llu settled_dts=%zu p90_err=%.2fms max_err=%.2fms subframes=%zu shift_nz=%lld\n",
            segNum, wantRepair, static_cast<unsigned long long>(seg.front().serial),
            static_cast<unsigned long long>(seg.back().serial), errs.size(), p90, maxErr, subs, static_cast<long long>(shiftNz));
        if (wantRepair) {
            expect(subs > 0 && shiftNz > 0 && p90 <= 5.0 && maxErr <= 8.0, "real-OBS pacing ON threshold failed");
        } else {
            expect(p90 >= 10.0, "real-OBS pacing OFF judder threshold failed");
        }
    };
    checkSegment(1, 0, 0, seg1EndIdx);
    checkSegment(2, 1, seg1EndIdx, seg2EndIdx);
    checkSegment(3, 0, seg2EndIdx, rows.size());
    std::puts("PASS libobs_pacing_repair_ab");
}

void frame_interval_120_case(const fs::path& log, const std::string& graphics)
{
    obs_video_info video{};
    video.graphics_module = graphics.c_str();
    video.fps_num = 120;
    video.fps_den = 1;
    video.base_width = video.output_width = width;
    video.base_height = video.output_height = height;
    video.output_format = VIDEO_FORMAT_RGBA;
    video.colorspace = VIDEO_CS_709;
    video.range = VIDEO_RANGE_FULL;
    expect(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "120/1 obs_reset_video");
    const auto interval = obs_get_frame_interval_ns();
    std::printf("120Hz obs_reset_video interval_ns=%llu\n", static_cast<unsigned long long>(interval));
    expect(interval == 8333333ULL, "120/1 OBS video tick must be 8333333 ns");

    TapLogCapture tapLog;
    auto input = source("vfg_test_color");
    auto data = settings();
    auto filter = source(filterId, data.get());
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    const auto offset = text_of(log).size();
    const auto props = properties(filter.get());
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_off"), filter.get()),
        "start 120Hz tap preview");
    const auto opened = tapLog.last("[vfg] open request ");
    expect(!opened.empty() && opened.find("transport_interval_ns=8333333") != std::string::npos &&
        opened.find("dedup_mode=1") != std::string::npos &&
        opened.find("live_anchor=1") != std::string::npos,
        "120Hz open request carries interval, dedup and live anchor");
    std::printf("120Hz open request: %s\n", opened.c_str());
    std::fflush(stdout);

    await([&] {
        std::istringstream stream(text_of(log).substr(offset));
        size_t count = 0;
        for (std::string line; std::getline(stream, line);)
            if (line.starts_with("event=present_original ")) ++count;
        return count >= 24;
    }, "120Hz presenter originals", 10000);

    struct Sample { uint64_t tick, qpc; };
    std::vector<Sample> samples;
    std::istringstream stream(text_of(log).substr(offset));
    for (std::string line; std::getline(stream, line);) {
        if (!line.starts_with("event=present_original ")) continue;
        const auto tick = field(line, "b_obs_tick=");
        const auto qpc = field(line, "qpc_ns=");
        if (!tick.empty() && !qpc.empty()) samples.push_back({std::stoull(tick), std::stoull(qpc)});
    }
    std::vector<int64_t> tickDeltas, qpcDeltas;
    bool monotonic = true;
    for (size_t i = 1; i < samples.size(); ++i) {
        const auto tickDelta = static_cast<int64_t>(samples[i].tick) - static_cast<int64_t>(samples[i - 1].tick);
        if (tickDelta <= 0) monotonic = false;
        tickDeltas.push_back(tickDelta);
        qpcDeltas.push_back(static_cast<int64_t>(samples[i].qpc) - static_cast<int64_t>(samples[i - 1].qpc));
    }
    std::sort(tickDeltas.begin(), tickDeltas.end());
    std::sort(qpcDeltas.begin(), qpcDeltas.end());
    const auto medianTick = tickDeltas[tickDeltas.size() / 2];
    const auto medianQpc = qpcDeltas[qpcDeltas.size() / 2];
    std::printf("120Hz delivery frames=%zu median_tick_delta_ns=%lld median_qpc_delta_ns=%lld\n",
        samples.size(), static_cast<long long>(medianTick), static_cast<long long>(medianQpc));
    std::fflush(stdout);
    expect(monotonic, "120Hz OBS ticks strictly advance");
    expect(medianTick >= 6'000'000 && medianTick <= 12'000'000, "120Hz OBS tick cadence near 8.33 ms");
    expect(medianQpc >= 6'000'000 && medianQpc <= 12'000'000, "120Hz presenter cadence near 8.33 ms");
    expect(std::llabs(medianTick - medianQpc) <= 2'000'000, "presenter follows the OBS 120Hz tick");

    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()),
        "stop 120Hz tap preview");
    obs_source_filter_remove(input.get(), filter.get());
    obs_queue_task(OBS_TASK_GRAPHICS, [](void*) {}, nullptr, true);
    std::puts("PASS 120Hz obs_reset_video tap delivery");
}

void run_isolated(wchar_t** argv, const Scenario& s)
{
    std::printf("START %ls\n", s.name);
    std::fflush(stdout);
    std::wstring cmd;
    for (int i = 0; i < 6; ++i) cmd += L"\"" + std::wstring(argv[i]) + L"\" ";
    cmd += s.name;
    const auto output = fs::path(argv[5]) / (std::wstring(s.name) + L".host.log");
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    Handle log{CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr)};
    expect(log.value != INVALID_HANDLE_VALUE, "capture isolated libobs stdout");
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = log.value;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    expect(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &pi) != FALSE, "start isolated libobs recovery case");
    const Handle process{pi.hProcess}, thread{pi.hThread};
    const auto closed = WaitForSingleObject(process.value, 14'000) == WAIT_OBJECT_0;
    if (!closed) TerminateProcess(process.value, 11);
    DWORD code{};
    const auto transcript = text_of(output);
    std::fputs(transcript.c_str(), stdout);
    expect(closed && GetExitCodeProcess(process.value, &code), "isolated libobs recovery case did not exit");
    if (code) throw std::runtime_error("isolated libobs recovery case exited " + std::to_string(code));
    constexpr char recovery[] = "[vfg] recovery requested ";
    const auto start = transcript.find(recovery);
    if (transcript.find(" relaunches=1 ") != std::string::npos) {
        expect(count_events(transcript, recovery) == 1, "one graphics enqueue for the single successful relaunch");
        const auto line = transcript.substr(start, transcript.find('\n', start) - start);
        const auto elapsed = field(line, "enqueue_ns=");
        expect(!elapsed.empty() && field(line, "graphics_thread=") == "1" &&
            std::stoull(elapsed) < 1'000'000, "graphics-thread recovery enqueue under 1 ms");
        std::printf("PASS enqueue %ls reason=%s enqueue_ns=%s\n", s.name, field(line, "reason=").c_str(), elapsed.c_str());
    } else expect(start == std::string::npos, "no graphics enqueue for zero-relaunch case");
}
}

void vfg_anr_fallback_case(const fs::path& fixture, const fs::path& log, bool stageCompanions);

int run_host(int argc, wchar_t** argv)
{
    try {
        expect(argc == 6 || argc == 7, "arguments: plugin plugin-data presenter graphics-module scratch [case]");
        fs::create_directories(argv[5]);
        if (argc == 6) {
            unsigned failed = 0;
            for (const auto& s : scenarios) {
                try { run_isolated(argv, s); }
                catch (const std::exception& e) {
                    std::fprintf(stderr, "FAIL %ls: %s\n", s.name, e.what());
                    ++failed;
                }
            }
            expect(failed == 0, "isolated libobs recovery failures");
            wchar_t pfBuf[4096]{};
            const auto pfLen = GetEnvironmentVariableW(L"OBS_NR_TEST_IMMEDIATE_REOPEN_RUNTIME", pfBuf, ARRAYSIZE(pfBuf));
            if (pfLen && pfLen < ARRAYSIZE(pfBuf)) {
                run_isolated(argv, Scenario{L"k-immediate-esc-reopen", Action::Escape});
                run_isolated(argv, Scenario{L"k-mutex-fail-fallback-unlock", Action::Escape});
            }
        }
        const auto plugin = std::filesystem::path(argv[1]).string();
        const auto data = std::filesystem::path(argv[2]).string();
        const auto graphics = std::filesystem::path(argv[4]).string();
        const auto scratch = std::filesystem::path(argv[5]) / std::to_wstring(GetCurrentProcessId());
        std::filesystem::create_directories(scratch);
        const auto log = scratch / "canvas-presenter.log";
        std::filesystem::remove(log);
        SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", argv[3]);
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
        struct Plugin { HMODULE value{}; ~Plugin() { if (value) FreeLibrary(value); } } pluginHandle;
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
        pluginHandle.value = LoadLibraryW(argv[1]);
        expect(pluginHandle.value != nullptr, "retain plugin through libobs teardown");
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
        info.get_width = [](void*) { return fixtureWidth.load(); };
        info.get_height = [](void*) { return height; };
        obs_register_source(&info);
        if (argc == 7) {
            SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());
            const std::wstring_view scenario = argv[6];
            if (scenario == L"properties") {
                properties_contract(fs::path(argv[2]), scratch, fs::path(argv[0]).parent_path() / L"anr_benign_fixture.dll");
                return 0;
            }
            if (scenario == L"anr-missing-companion" || scenario == L"anr-missing-pack") {
                vfg_anr_fallback_case(fs::path(argv[0]).parent_path() / L"anr_benign_fixture.dll", log,
                    scenario == L"anr-missing-pack");
                return 0;
            }
            if (scenario == L"pacing-repair-ab") {
                wchar_t rtBuf[4096]{};
                if (!GetEnvironmentVariableW(L"OBS_NR_VFG_RUNTIME", rtBuf, ARRAYSIZE(rtBuf))) {
                    const auto siblingRt = fs::path(argv[4]).parent_path() / L"nvidia-vfg";
                    SetEnvironmentVariableW(L"OBS_NR_VFG_RUNTIME", siblingRt.c_str());
                }
                pacing_repair_ab_case(log);
                return 0;
            }
            if (scenario == L"k-immediate-esc-reopen") {
                wchar_t pfBuf[4096]{};
                const auto pfLen = GetEnvironmentVariableW(L"OBS_NR_TEST_IMMEDIATE_REOPEN_RUNTIME", pfBuf, ARRAYSIZE(pfBuf));
                expect(pfLen && pfLen < ARRAYSIZE(pfBuf), "OBS_NR_TEST_IMMEDIATE_REOPEN_RUNTIME required");
                immediate_esc_reopen_case(scratch, log, std::wstring(pfBuf, pfLen));
                immediate_esc_reopen_stalled_diag_case(scratch, scratch / L"reopen-diag-stalled.log", std::wstring(pfBuf, pfLen));
                return 0;
            }
            if (scenario == L"k-mutex-fail-fallback-unlock") {
                wchar_t pfBuf[4096]{};
                const auto pfLen = GetEnvironmentVariableW(L"OBS_NR_TEST_IMMEDIATE_REOPEN_RUNTIME", pfBuf, ARRAYSIZE(pfBuf));
                expect(pfLen && pfLen < ARRAYSIZE(pfBuf), "OBS_NR_TEST_IMMEDIATE_REOPEN_RUNTIME required");
                mutex_failure_fallback_unlocks_tap_case(scratch, scratch / L"mutex-fail-fallback.log", std::wstring(pfBuf, pfLen));
                return 0;
            }
            SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", (scratch / L"missing-runtime").c_str());
            for (const auto& s : scenarios) if (wcscmp(argv[6], s.name) == 0) {
                recovery_case(scratch, s);
                return 0;
            }
            throw std::runtime_error("unknown recovery scenario");
        }
        canvas_contract(log);
        frame_interval_120_case(log, graphics);
        std::puts("PASS vfg_filter_output: RGBA zero, scene blend, preview feed and restore");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_filter_output: %s\n", e.what());
        return 1;
    }
}

int wmain(int argc, wchar_t** argv)
{
    const auto result = run_host(argc, argv);
    if (argc == 7) {
        // This isolated process may deliberately retain two quarantined
        // GPU generations. Do not unload their driver after libobs shutdown.
        // Reclaim them with the process, keeping the real scenario exit code.
        std::fflush(stdout);
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
    }
    return result;
}
