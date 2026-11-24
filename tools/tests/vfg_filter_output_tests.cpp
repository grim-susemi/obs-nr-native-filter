// SPDX-License-Identifier: GPL-3.0-only
#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/text-lookup.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>

std::filesystem::path vfg_stage_anr_addon(const std::filesystem::path& fixture,
    const std::filesystem::path& addon, bool stageCompanions);

namespace {
constexpr uint32_t width = 320, height = 180;
std::atomic_uint32_t fixtureWidth{width};
std::atomic_bool fixtureBlue{};
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
    const vec4 color = fixtureBlue.load() ? vec4{0.f, 0.f, 1.f, 1.f} : vec4{1.f, 0.f, 0.f, 1.f};
    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, fixtureWidth.load(), height);
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
    for (const bool legacy : {false, true}) {
        auto values = settings();
        if (legacy) obs_data_set_int(values.get(), "vfg_quality_mode", 1);
        auto filter = source(filterId, values.get());
        obs_source_load(filter.get());
        const auto saved = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(
            obs_source_get_settings(filter.get()), obs_data_release);
        for (const auto& [key, expected] : std::initializer_list<std::pair<const char*, int>>{
                 {"vfg_multiplier", 2}, {"vfg_quality_mode", 1}, {"vfg_fps_overlay", 1},
                 {"vfg_backend", 0}, {"vfg_pushflow_tier", 0}, {"vfg_adaptive_target_fps", 0},
                 {"cas_strength", 35}, {"scaling_backend", 0}, {"dlss_model", 1}, {"split_compare_position", 50}})
            expect(obs_data_get_int(saved.get(), key) == expected, key);
        for (const auto* key : {"vfg_low_latency", "vfg_gpu_priority"})
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
        if (s.closeRecovered)
            expect(text_of(quarantine) == "reason=controller_unexpected_exit\n" &&
                !event(text_of(log), "event=shutdown_force_terminate watchdog=0 exit_reason=1").empty(),
                "forced Esc in recovered 1x preserves quarantine");
    } else {
        if (oldExit != 0)
            throw std::runtime_error("user/parent close exited " + std::to_string(oldExit));
        if (s.stall || (s.action == Action::Stop && s.closeRecovered)) expect(
            !event(text_of(log), "event=shutdown_force_terminate watchdog=0 exit_reason=1").empty(), "stalled user close preserves UserClosed");
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
        if (earlyCheckpoint) expect(oldOffer.empty(), "early checkpoint has no prior offer");
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
            SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", (scratch / L"missing-runtime").c_str());
            for (const auto& s : scenarios) if (wcscmp(argv[6], s.name) == 0) {
                recovery_case(scratch, s);
                return 0;
            }
            throw std::runtime_error("unknown recovery scenario");
        }
        canvas_contract(log);
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
