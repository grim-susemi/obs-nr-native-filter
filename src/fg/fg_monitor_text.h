// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdint>
#include <string>

namespace nr::fg::vfg {
inline std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n)) return {};
    return out;
}

inline std::string wide_to_utf8(const wchar_t* s)
{
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr)) return {};
    out.pop_back();
    return out;
}

// The OBS property sheet and its contract tests share one pure UI policy. The
// backend numbers mirror Backend in fg_vfg_session.h; fg_tap.cpp proves that
// mapping with static_assert so the UI can never silently target another
// backend when the enum moves.
namespace ui {
inline constexpr uint32_t backend_native_vfg = 0;
inline constexpr uint32_t backend_pushflow = 1;
inline constexpr uint32_t backend_anr = 2;
inline constexpr uint32_t backend_xefg = 3;
inline constexpr uint32_t backend_xemfg = 4;
inline constexpr uint32_t multiplier_min = 2;
inline constexpr uint32_t multiplier_max = 4;

struct Defaults {
    uint32_t backend = backend_native_vfg;
    uint32_t multiplier = multiplier_min;
    bool low_latency = true;
    bool xemfg_opt_in = false;
    bool shared_nvof = true;
};
inline constexpr Defaults defaults{};

struct Selection {
    uint32_t backend = backend_native_vfg;
    uint32_t requested_multiplier = multiplier_min;
    bool xemfg_opt_in = false;
    bool xemfg_path = false;
    bool xess_runtime = false;
    bool low_latency = true;
};

enum class Refusal : uint32_t { none = 0, xemfg_opt_in, xemfg_path };

struct Policy {
    uint32_t multiplier_limit = multiplier_max;
    uint32_t effective_multiplier = multiplier_min;
    bool can_launch = true;
    bool split_compare_enabled = true;
    bool hud_lock_enabled = true;
    bool xemfg_opt_in_visible = false;
    bool xemfg_path_visible = false;
    Refusal refusal = Refusal::none;
    uint32_t reported_low_latency = 0;
    const char* multiplier_reason_key = nullptr;
    const char* split_reason_key = nullptr;
    const char* hud_lock_reason_key = nullptr;
    const char* low_latency_reason_key = nullptr;
    const char* refusal_key = nullptr;
};

inline uint32_t clamp_multiplier(uint32_t requested, uint32_t limit) noexcept
{
    if (requested < multiplier_min) return requested;
    return requested > limit ? limit : requested;
}

inline Policy policy(const Selection& selection) noexcept
{
    Policy result;
    result.xemfg_opt_in_visible = selection.backend == backend_xemfg;
    result.xemfg_path_visible = result.xemfg_opt_in_visible && selection.xemfg_opt_in;
    switch (selection.backend) {
    case backend_xefg:
        // The official 2x presenter owns a proxy swapchain: no post-compose
        // HUD lock, no split compare, and no UI value can disable XeLL sleep.
        result.multiplier_limit = 2;
        result.split_compare_enabled = false;
        result.split_reason_key = "VFG.Status.SplitCompareXeFg";
        result.hud_lock_enabled = false;
        result.hud_lock_reason_key = "VFG.Status.HudLockXeFg";
        break;
    case backend_xemfg:
        if (!selection.xemfg_opt_in) {
            result.refusal = Refusal::xemfg_opt_in;
            result.refusal_key = "VFG.Status.XeMfg.OptInRequired";
        } else if (!selection.xemfg_path) {
            result.refusal = Refusal::xemfg_path;
            result.refusal_key = "VFG.Status.XeMfg.Select";
        }
        break;
    case backend_native_vfg:
    case backend_pushflow:
    case backend_anr:
        break;
    default:
        break;
    }
    result.effective_multiplier = clamp_multiplier(selection.requested_multiplier, result.multiplier_limit);
    result.can_launch = result.refusal == Refusal::none;
    if (selection.backend == backend_xefg) {
        if (selection.requested_multiplier > result.multiplier_limit)
            result.multiplier_reason_key = "VFG.Status.MultiplierCapped";
        if (result.effective_multiplier > 1) {
            // XeFgPresenter calls xellSetSleepMode(1) and enables latency
            // reduction for every UI value, so the status must never imply the
            // toggle can turn it off. A refused/1x session reports 0 below.
            result.low_latency_reason_key = "VFG.Status.LowLatencyForced";
            if (selection.xess_runtime) result.reported_low_latency = 1;
        }
    } else if (result.can_launch && result.effective_multiplier > 1 && selection.low_latency) {
        result.reported_low_latency = 1;
    }
    return result;
}

inline const char* backend_key(uint32_t backend) noexcept
{
    switch (backend) {
    case backend_pushflow: return "VFG.Backend.PushFlow";
    case backend_anr: return "VFG.Backend.ANR";
    case backend_xefg: return "VFG.Backend.XeFg";
    case backend_xemfg: return "VFG.Backend.XeMfg";
    case backend_native_vfg:
    default: return "VFG.Backend.Native";
    }
}

inline const char* status_state_key(bool preview_open, bool refused) noexcept
{
    if (refused) return "VFG.Status.Refused";
    return preview_open ? "VFG.Status.PreviewOpen" : "VFG.Status.NextOpen";
}

enum class SharedNvofState : uint32_t {
    unavailable = 0,
    shared_from_nr,
    local_fallback,
    nr_owned_waiting,
};

struct SharedNvofInput {
    bool launched = false;
    bool shared_enabled = true;
    bool nr_present = false;
    bool nr_enabled = false;
    uint64_t frames = 0;
    uint64_t hits = 0;
};

inline SharedNvofState shared_nvof_state(const SharedNvofInput& input) noexcept
{
    if (!input.launched) return SharedNvofState::unavailable;
    if (!input.shared_enabled) return SharedNvofState::local_fallback;
    if (input.nr_present && input.nr_enabled)
        return input.frames && input.hits ? SharedNvofState::shared_from_nr : SharedNvofState::nr_owned_waiting;
    return SharedNvofState::local_fallback;
}

inline const char* shared_nvof_key(SharedNvofState state) noexcept
{
    switch (state) {
    case SharedNvofState::shared_from_nr: return "VFG.SharedNvof.FromNr";
    case SharedNvofState::local_fallback: return "VFG.SharedNvof.LocalFallback";
    case SharedNvofState::nr_owned_waiting: return "VFG.SharedNvof.Pending";
    case SharedNvofState::unavailable:
    default: return "VFG.SharedNvof.Unavailable";
    }
}

// The exact text the property sheet renders, shared with the contract tests.
// A NULL property param used to force the unavailable default; this function
// starts from the observed input instead.
template <class TextLookup>
inline std::string shared_nvof_status_text(const SharedNvofInput& input, TextLookup&& text)
{
    const auto state = shared_nvof_state(input);
    std::string line = text(shared_nvof_key(state));
    if (state == SharedNvofState::shared_from_nr || state == SharedNvofState::nr_owned_waiting) {
        line += " (";
        line += std::to_string(input.hits);
        line += "/";
        line += std::to_string(input.frames);
        line += ")";
    }
    if (!input.shared_enabled) {
        line += " | ";
        line += text("VFG.SharedNvof.Off");
    }
    return line;
}
} // namespace ui
} // namespace nr::fg::vfg
