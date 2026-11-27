// SPDX-License-Identifier: GPL-3.0-only
#include "fg_monitor_text.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

namespace {
using nr::fg::vfg::ui::SharedNvofState;

bool contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

std::string unquote(std::string value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

std::map<std::string, std::string> read_locale(const std::filesystem::path& path)
{
    std::map<std::string, std::string> out;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        out[key] = unquote(line.substr(eq + 1));
    }
    return out;
}

std::filesystem::path locale_directory()
{
    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    for (int depth = 0; !ec && depth < 6; ++depth) {
        if (std::filesystem::exists(dir / "data" / "locale" / "en-US.ini", ec))
            return dir / "data" / "locale";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

int fail(const char* reason)
{
    std::fprintf(stderr, "FAIL vfg_monitor_text: %s\n", reason);
    return 1;
}
} // namespace

int main(int argc, char** argv)
{
    using nr::fg::vfg::utf8_to_wide;
    using nr::fg::vfg::wide_to_utf8;
    const std::wstring ids[]{
        LR"(\\?\DISPLAY#ACR1234#5&123456&0&UID1234#{abcdef})",
        L"\\\\.\\DISPLAY2",
        L"monitor-\uD55C\uAD6D",
    };
    for (const auto& id : ids) {
        const auto encoded = wide_to_utf8(id.c_str());
        if (encoded.empty() || encoded.find('\0') != std::string::npos || utf8_to_wide(encoded) != id)
            return fail("monitor ID round trip");
    }
    if (!wide_to_utf8(L"").empty() || !utf8_to_wide("").empty())
        return fail("auto selection must remain empty");

    using namespace nr::fg::vfg::ui;
    if (defaults.backend != backend_native_vfg || defaults.multiplier != 2 || !defaults.low_latency ||
        defaults.xemfg_opt_in || !defaults.shared_nvof)
        return fail("UI defaults");

    const auto xefg = policy(Selection{backend_xefg, 4, false, false, true, false});
    if (xefg.multiplier_limit != 2 || xefg.effective_multiplier != 2 || !xefg.can_launch ||
        xefg.split_compare_enabled || xefg.hud_lock_enabled || !xefg.split_reason_key ||
        !xefg.hud_lock_reason_key || !xefg.multiplier_reason_key || !xefg.low_latency_reason_key ||
        xefg.reported_low_latency != 1)
        return fail("XeFG 2x clamp, split/HUD gating, or forced low latency");
    const auto xefg1x = policy(Selection{backend_xefg, 1, false, false, true, true});
    if (xefg1x.effective_multiplier != 1 || xefg1x.reported_low_latency != 0)
        return fail("XeFG 1x must not report low_latency=1");
    const auto xefgMissing = policy(Selection{backend_xefg, 2, false, false, false, true});
    if (xefgMissing.reported_low_latency != 0 || !xefgMissing.low_latency_reason_key)
        return fail("XeFG without the runtime reports no running low latency");
    const auto xemfgOptOut = policy(Selection{backend_xemfg, 4, false, false, true, true});
    if (xemfgOptOut.can_launch || xemfgOptOut.refusal != Refusal::xemfg_opt_in || !xemfgOptOut.refusal_key ||
        xemfgOptOut.reported_low_latency != 0)
        return fail("XeMFG opt-in refusal");
    const auto xemfgPath = policy(Selection{backend_xemfg, 4, true, false, true, true});
    if (xemfgPath.can_launch || xemfgPath.refusal != Refusal::xemfg_path || !xemfgPath.refusal_key)
        return fail("XeMFG DLL path refusal");
    const auto xemfgReady = policy(Selection{backend_xemfg, 4, true, true, true, true});
    if (!xemfgReady.can_launch || xemfgReady.effective_multiplier != 4 || !xemfgReady.split_compare_enabled ||
        !xemfgReady.hud_lock_enabled || xemfgReady.reported_low_latency != 1)
        return fail("XeMFG 4x keeps split compare, HUD lock, and low latency");

    if (shared_nvof_state({false, true, true, true, 1, 1}) != SharedNvofState::unavailable ||
        shared_nvof_state({true, false, true, true, 1, 1}) != SharedNvofState::local_fallback ||
        shared_nvof_state({true, true, false, false, 1, 1}) != SharedNvofState::local_fallback ||
        shared_nvof_state({true, true, true, true, 4, 4}) != SharedNvofState::shared_from_nr ||
        shared_nvof_state({true, true, true, true, 4, 0}) != SharedNvofState::nr_owned_waiting)
        return fail("Shared NVOF state table");

    const auto locale = argc > 1 ? std::filesystem::path(argv[1]) : locale_directory();
    if (locale.empty()) return fail("locale directory not found");
    const auto en = read_locale(locale / "en-US.ini");
    const auto ko = read_locale(locale / "ko-KR.ini");
    if (en.empty() || en.size() != ko.size()) return fail("en-US/ko-KR key count parity");
    for (const auto& [key, value] : en) {
        if (!ko.contains(key) || value.empty() || ko.at(key).empty())
            return fail(key.c_str());
    }
    for (const auto& [key, value] : ko) {
        if (!en.contains(key) || value.empty())
            return fail(key.c_str());
    }
    for (const char* key : {"VFG.Backend.XeFg", "VFG.Backend.XeMfg", "VFG.XeMfg.OptIn", "VFG.XeMfg.DllPath",
             "VFG.XeMfg.Warning", "VFG.SharedNvof", "VFG.SharedNvof.Status", "VFG.SharedNvof.FromNr",
             "VFG.SharedNvof.LocalFallback", "VFG.SharedNvof.Unavailable", "VFG.SharedNvof.Pending",
             "VFG.XessRuntimeInfo", "VFG.Status.XeFg.Ready", "VFG.Status.XeFg.RuntimeMissing",
             "VFG.Status.XeMfg.OptInRequired", "VFG.Status.XeMfg.Select", "VFG.Status.XeMfg.Missing",
             "VFG.Status.XeMfg.InvalidPath",
             "VFG.Status.XeMfg.Ready", "VFG.Status.SplitCompareXeFg", "VFG.Status.HudLockXeFg",
             "VFG.Status.MultiplierCapped", "VFG.Status.LowLatencyForced", "VFG.Status.NextOpen",
             "VFG.Status.PreviewOpen", "VFG.Status.Refused", "VFG.Status.Requested", "VFG.Status.FallbackNote",
             "VFG.Status.NoPresenterAbove1x", "VFG.Hint.MultiplierXeFg", "VFG.Hint.LowLatencyXeFg"}) {
        if (!en.contains(key) || !ko.contains(key)) return fail(key);
    }
    if (en.at("VFG.Backend.XeFg") != "XeFG (Intel 3.0.2, 2x)" ||
        en.at("VFG.Backend.XeMfg") != "XeMFG (3x/4x Opt-in)" ||
        en.at("VFG.SharedNvof.FromNr") != "Shared from NR (OFA 0 local)" ||
        en.at("VFG.SharedNvof.LocalFallback") != "Local NVOF fallback" ||
        !contains(en.at("VFG.SharedNvof.Unavailable"), "Unavailable") ||
        !contains(en.at("VFG.Status.LowLatencyForced"), "forced"))
        return fail("exact English status strings");
    const auto locale_text = [&](const char* key) { return en.at(key); };
    if (shared_nvof_status_text({true, true, true, true, 120, 120}, locale_text) !=
            "Shared from NR (OFA 0 local) (120/120)" ||
        shared_nvof_status_text({true, true, true, true, 4, 0}, locale_text) !=
            "NR-owned (pair pending; OFA 0 local) (0/4)" ||
        shared_nvof_status_text({true, true, false, false, 0, 0}, locale_text) != "Local NVOF fallback" ||
        shared_nvof_status_text({true, false, true, true, 4, 4}, locale_text) !=
            "Local NVOF fallback | Shared NVOF is off" ||
        shared_nvof_status_text({false, true, false, false, 0, 0}, locale_text) != "Unavailable (preview closed)")
        return fail("Shared NVOF status rendering");
    if (ko.at("VFG.SharedNvof.FromNr") == en.at("VFG.SharedNvof.FromNr") ||
        ko.at("VFG.Status.LowLatencyForced") == en.at("VFG.Status.LowLatencyForced"))
        return fail("Korean status strings are not untranslated copies");
    std::puts("PASS vfg_monitor_text");
    return 0;
}
