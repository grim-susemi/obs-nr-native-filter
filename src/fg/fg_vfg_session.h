// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_ipc.h"
#include <cstdio>

namespace nr::fg::vfg {
inline constexpr uint32_t max_multiplier = 4;
inline constexpr uint32_t session_version = 4;
inline constexpr uint32_t xefg_stability_mask_bits = 0x7;
enum class DedupMode : uint32_t { OffExact = 0, Auto = 1, Adaptive60To30 = 5, Fixed60To30 = 6 };
inline constexpr bool is_valid_dedup_mode(uint32_t mode) noexcept
{
    switch (static_cast<DedupMode>(mode)) {
    case DedupMode::OffExact:
    case DedupMode::Auto:
    case DedupMode::Adaptive60To30:
    case DedupMode::Fixed60To30:
        return true;
    }
    return false;
}
inline constexpr bool is_valid_live_anchor(uint32_t anchor) noexcept
{
    return anchor <= 1;
}
inline std::wstring format_marker_token_w(uint64_t token)
{
    wchar_t buf[24]{};
    swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(token));
    return buf;
}
inline std::wstring starting_marker_filename(uint64_t ownerToken)
{
    if (ownerToken == 0) return L"pushflow-starting.txt";
    return L"pushflow-starting-" + format_marker_token_w(ownerToken) + L".txt";
}
inline std::wstring marker_mutex_name(uint64_t ownerToken)
{
    return L"Local\\obs-nr-pf-tok-" + format_marker_token_w(ownerToken);
}
enum class Backend : uint32_t { NativeVfg = 0, PushFlow = 1, AnrExperimental = 2, XeFg = 3, XeMfg = 4 };
enum class PushFlowTier : uint32_t { Fast = 0, Quality = 1 };
enum class ExitReason : LONG { None = 0, UserClosed = 1, Fault = 2, Watchdog = 3 };
inline constexpr bool is_valid_backend(Backend backend) noexcept
{
    switch (backend) {
    case Backend::NativeVfg:
    case Backend::PushFlow:
    case Backend::AnrExperimental:
    case Backend::XeFg:
    case Backend::XeMfg:
        return true;
    }
    return false;
}
inline constexpr bool is_valid_switch(uint32_t on) noexcept
{
    return on <= 1;
}
inline constexpr const char* backend_name(Backend backend) noexcept
{
    switch (backend) {
    case Backend::NativeVfg: return "nvidia-vfg-native";
    case Backend::PushFlow: return "pushflow";
    case Backend::AnrExperimental: return "anr-experimental";
    case Backend::XeFg: return "xefg";
    case Backend::XeMfg: return "xemfg";
    }
    return "invalid";
}
enum class Quality : uint32_t { Low = 0, Medium = 1, High = 2 };
inline constexpr Quality decode_quality(long long mode) noexcept
{
    return mode == 0 ? Quality::Low : mode == 2 ? Quality::High : Quality::Medium;
}

struct Session {
    uint32_t sessionVersion = session_version;
    uint32_t sessionBytes = sizeof(Session);
    Backend backend{};
    PushFlowTier pushflowTier{};
    uint32_t gpuShotChange{}, experimentalAnrOptIn{};
    uint64_t launchEpoch = 1; // A recovery child must not replay first-launch fixtures.
    uint64_t markerOwnerToken{}; // Non-zero for PushFlow multiplier > 1 sessions to isolate starting markers.
    wchar_t pushflowRuntimeDir[520]{}, anrDllPath[520]{}, xessRuntimeDir[520]{}, xemfgDllPath[520]{};
    alignas(4) volatile LONG experimentalXeMfgOptIn{}, xefgStabilityMask{}, sharedNvofEnabled{};
    alignas(4) volatile LONG hudLock{}, pacingRepair{}, adaptiveFg{}, adaptiveTargetFps{}, exitReason{};
    alignas(4) volatile LONG forceTerminated{}, watchdogInitializing{}, pushflowActive{}, drainFailed{}, logWriteInFlight{};
    alignas(8) volatile LONG64 watchdogElapsedMs{};
    alignas(8) volatile LONG64 state; // 0 initializing, 1 offered, 2 drain for resize, 3 drained, -1 stop
    uint64_t generation;
    uint32_t width, height;
    uint32_t multiplier; // 1 = original preview, 2..4 = VFG interpolated preview
    uint32_t quality;    // NVIDIA VFG Mode (Quality)
    LUID adapter;
    wchar_t mapping[112];
    wchar_t monitorId[128]; // Empty = auto display selection
    alignas(4) volatile LONG fpsOverlay;  // 0 = off, 1 = top-left, 2 = top-right
    alignas(4) volatile LONG lowLatency;  // 0 = off, 1 = NVIDIA Reflex on
    alignas(4) volatile LONG gpuPriority; // 0 = normal, 1 = high
    alignas(4) volatile LONG hideFromCapture; // 0 = capturable (WDA_NONE), 1 = hidden (WDA_EXCLUDEFROMCAPTURE = 0x11)
    uint32_t splitCompare; // Fixed for the lifetime of each offered generation.
    alignas(4) volatile LONG dedupMode{0}; // DedupMode: 0 = off/exact, 1 = auto, 5 = 60->30 adaptive, 6 = 60->30 fixed
    alignas(4) volatile LONG liveAnchor{0}; // 0 = off, 1 = live clock phase-lock
    uint64_t transportIntervalNs{0}; // OBS video tick in ns; 0 = presenter default pacing
};
inline bool validate_session_header(const Session& s) noexcept
{
    if (s.sessionVersion != session_version || s.sessionBytes != sizeof(Session) ||
        !is_valid_backend(s.backend) ||
        (s.pushflowTier != PushFlowTier::Fast && s.pushflowTier != PushFlowTier::Quality) ||
        !is_valid_dedup_mode(static_cast<uint32_t>(s.dedupMode)) ||
        !is_valid_live_anchor(static_cast<uint32_t>(s.liveAnchor)) ||
        !is_valid_switch(static_cast<uint32_t>(s.experimentalAnrOptIn)) ||
        !is_valid_switch(static_cast<uint32_t>(s.experimentalXeMfgOptIn)) ||
        !is_valid_switch(static_cast<uint32_t>(s.sharedNvofEnabled)) ||
        (static_cast<uint32_t>(s.xefgStabilityMask) & ~xefg_stability_mask_bits) ||
        !wmemchr(s.pushflowRuntimeDir, L'\0', ARRAYSIZE(s.pushflowRuntimeDir)) ||
        !wmemchr(s.anrDllPath, L'\0', ARRAYSIZE(s.anrDllPath)) ||
        !wmemchr(s.xessRuntimeDir, L'\0', ARRAYSIZE(s.xessRuntimeDir)) ||
        !wmemchr(s.xemfgDllPath, L'\0', ARRAYSIZE(s.xemfgDllPath))) return false;
    // A multiplier-1 recovery header is always legal: run_fallback clears the
    // experimental runtime paths and opt-in when it degrades to the original frame.
    if (s.multiplier == 1) return true;
    switch (s.backend) {
    case Backend::NativeVfg:
    case Backend::PushFlow:
        return true;
    case Backend::AnrExperimental:
        return s.experimentalAnrOptIn == 1 && s.anrDllPath[0];
    case Backend::XeFg:
        return s.xessRuntimeDir[0] != L'\0';
    case Backend::XeMfg:
        return s.experimentalXeMfgOptIn == 1 && s.xemfgDllPath[0] && s.xessRuntimeDir[0];
    }
    return false;
}
inline ExitReason read_exit_reason(Session& s) noexcept
{
    return static_cast<ExitReason>(InterlockedCompareExchange(&s.exitReason, 0, 0));
}
inline void stamp_exit_reason(Session& s, ExitReason reason) noexcept
{
    InterlockedCompareExchange(&s.exitReason, static_cast<LONG>(reason), 0);
}
inline constexpr DWORD display_affinity(LONG hideFromCapture) noexcept
{
    return hideFromCapture ? 0x11u : 0x0u;
}
inline constexpr bool should_hide_source_canvas(bool hideSourceCanvas, bool vfgActive) noexcept
{
    return hideSourceCanvas && vfgActive;
}
inline uint64_t clock_ns() noexcept
{
    LARGE_INTEGER tick{}, rate{};
    QueryPerformanceCounter(&tick);
    QueryPerformanceFrequency(&rate);
    const auto t = static_cast<uint64_t>(tick.QuadPart), f = static_cast<uint64_t>(rate.QuadPart);
    return t / f * 1000000000ULL + t % f * 1000000000ULL / f;
}
} // namespace nr::fg::vfg
