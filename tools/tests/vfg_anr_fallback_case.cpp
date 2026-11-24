// SPDX-License-Identifier: GPL-3.0-only
#include <obs-module.h>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
namespace fs = std::filesystem;
constexpr std::string_view missingPack = "programs/fg-lite-any-dp4a-srgb8/pack.nrx";
constexpr std::string_view presentCompanions[]{
    "nr_fg.ini", "path_table.json", "size_table.json",
    "adapters/pack.nrx", "kernels/pack.nrx", "kernels-portable/pack.nrx",
    "programs/fg-lite-any-coopmat-pq10/pack.nrx", "programs/fg-lite-any-coopmat-srgb8/pack.nrx",
    "programs/fg-lite-any-dp4a-pq10/pack.nrx",
    "programs/fg-lite-any-packed-pq10/pack.nrx", "programs/fg-lite-any-packed-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-linalg-pq10/pack.nrx", "programs/fg-lite-any-portable-linalg-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-pq10/pack.nrx", "programs/fg-lite-any-portable-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-xmx-pq10/pack.nrx", "programs/fg-lite-any-portable-xmx-srgb8/pack.nrx",
};

void expect(bool pass, const char* message)
{
    if (!pass) throw std::runtime_error(message);
}

struct Observations {
    size_t diagnostics{}, sessions{}, oneXOffers{}, originals{}, synthetic{};
    uint64_t firstOriginalNs{}, lastOriginalNs{};

    bool sustained() const
    {
        return originals >= 8 && lastOriginalNs > firstOriginalNs &&
            lastOriginalNs - firstOriginalNs >= 250'000'000;
    }
};

uint64_t number_field(std::string_view line, std::string_view token)
{
    const auto pos = line.find(token);
    expect(pos != line.npos, "missing presentation telemetry field");
    const auto value = line.substr(pos + token.size());
    const auto end = value.find(' ');
    const auto last = value.data() + (end == value.npos ? value.size() : end);
    uint64_t number{};
    const auto result = std::from_chars(value.data(), last, number);
    expect(result.ec == std::errc{} && result.ptr == last, "invalid presentation telemetry field");
    return number;
}

Observations observe(const fs::path& log, std::string_view diagnostic)
{
    std::ifstream input(log);
    Observations result;
    for (std::string line; std::getline(input, line);) {
        if (input.eof()) break; // The presenter may still be writing the final line.
        if (line == diagnostic) ++result.diagnostics;
        if (line.starts_with("event=session_started ")) {
            expect(line.starts_with("event=session_started backend=anr-experimental ") &&
                line.ends_with(" epoch=1"), "expected one initial ANR session");
            ++result.sessions;
        }
        if (line.starts_with("event=offer ")) {
            expect(line.starts_with("event=offer mode=1x ") &&
                line.ends_with(" backend=anr-experimental epoch=1"), "Controller must offer ANR at 1x");
            ++result.oneXOffers;
        }
        if (line.starts_with("event=present_vfg_subframe ")) ++result.synthetic;
        if (line.starts_with("event=present_original ")) {
            expect(number_field(line, " eff_mult=") == 1, "original frame multiplier must be 1x");
            if (!number_field(line, " present_id=")) continue;
            const auto timestamp = number_field(line, " qpc_ns=");
            if (!result.originals) result.firstOriginalNs = timestamp;
            result.lastOriginalNs = timestamp;
            ++result.originals;
        }
    }
    return result;
}
} // namespace

fs::path vfg_stage_anr_addon(const fs::path& fixture, const fs::path& addon, bool stageCompanions)
{
    expect(fs::create_directory(addon), "create isolated addon directory");
    const auto dll = (addon / L"nvngx_dlssg.dll").make_preferred();
    fs::copy_file(fixture, dll);
    if (stageCompanions) {
        fs::create_directories((addon / fs::path(missingPack)).parent_path());
        for (const auto relative : presentCompanions) {
            const auto file = addon / fs::path(relative);
            fs::create_directories(file.parent_path());
            std::ofstream ordinary;
            ordinary.exceptions(std::ios::failbit | std::ios::badbit);
            ordinary.open(file, std::ios::binary);
            ordinary << "owned ANR fallback test companion\n";
            ordinary.close();
        }
    }
    return dll;
}

void vfg_anr_fallback_case(const fs::path& fixture, const fs::path& log, bool stageCompanions)
{
    const auto addon = log.parent_path() / (stageCompanions ? L"missing-pack-anr" : L"standalone-anr");
    const auto dll = vfg_stage_anr_addon(fixture, addon, stageCompanions);
    const auto diagnostic = std::string("event=anr_validation_fallback reason=incomplete_anr_addon_missing_companions missing=") +
        std::string(stageCompanions ? missingPack : "nr_fg.ini");
    const auto utf8 = dll.u8string();
    const std::string dllSetting(utf8.begin(), utf8.end());
    const auto data = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(obs_data_create(), obs_data_release);
    obs_data_set_int(data.get(), "vfg_backend", 2);
    obs_data_set_int(data.get(), "vfg_multiplier", 2);
    obs_data_set_bool(data.get(), "vfg_experimental_anr", true);
    obs_data_set_string(data.get(), "vfg_anr_dll_path", dllSetting.c_str());
    using Source = std::unique_ptr<obs_source_t, decltype(&obs_source_release)>;
    const Source input{obs_source_create_private("vfg_test_color", "ANR fallback input", nullptr), obs_source_release};
    const Source filter{obs_source_create_private("susemi_fg_tap", "ANR fallback filter", data.get()), obs_source_release};
    expect(input && filter && obs_obj_get_data(filter.get()), "create ANR filter and source");
    obs_source_filter_add(input.get(), filter.get());
    obs_set_output_source(0, input.get());
    struct ClearOutput { ~ClearOutput() { obs_set_output_source(0, nullptr); } } clearOutput;
    const auto props = std::unique_ptr<obs_properties_t, decltype(&obs_properties_destroy)>(
        obs_source_properties(filter.get()), obs_properties_destroy);
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_start"), filter.get()),
        "request ANR preview with a 2x setting");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto running = observe(log, diagnostic);
        if (running.synthetic || running.sustained()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    expect(obs_property_button_clicked(obs_properties_get(props.get(), "vfg_stop"), filter.get()), "stop ANR preview");
    obs_queue_task(OBS_TASK_GRAPHICS, [](void*) {}, nullptr, true);
    obs_source_filter_remove(input.get(), filter.get());
    const auto final = observe(log, diagnostic);
    std::printf("ANR fallback diagnostics=%zu sessions=%zu 1x_offers=%zu originals=%zu synthetic=%zu span_ns=%llu\n",
        final.diagnostics, final.sessions, final.oneXOffers, final.originals, final.synthetic,
        static_cast<unsigned long long>(final.lastOriginalNs - final.firstOriginalNs));
    expect(final.diagnostics == 1 && final.sessions == 1 && final.oneXOffers == 1,
        "exact missing-companion diagnostic and single initial 1x ANR offer");
    expect(final.sustained() && final.synthetic == 0, "sustained original-only presentations");
}
