// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_ipc.h"

namespace nr::fg::vfg {
inline constexpr uint32_t max_multiplier = 4;
inline constexpr uint32_t session_version = 2;
enum class Backend : uint32_t { NativeVfg = 0, PushFlow = 1, AnrExperimental = 2 };
enum class PushFlowTier : uint32_t { Fast = 0, Quality = 1 };
enum class ExitReason : LONG { None = 0, UserClosed = 1, Fault = 2, Watchdog = 3 };
inline constexpr const char* backend_name(Backend backend) noexcept
{
    switch (backend) {
    case Backend::NativeVfg: return "nvidia-vfg-native";
    case Backend::PushFlow: return "pushflow";
    case Backend::AnrExperimental: return "anr-experimental";
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
    wchar_t pushflowRuntimeDir[520]{}, anrDllPath[520]{};
    alignas(4) volatile LONG hudLock{}, pacingRepair{}, adaptiveFg{}, adaptiveTargetFps{}, exitReason{};
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
};
inline bool validate_session_header(const Session& s) noexcept
{
    if (s.sessionVersion != session_version || s.sessionBytes != sizeof(Session) ||
        (s.backend != Backend::NativeVfg && s.backend != Backend::PushFlow && s.backend != Backend::AnrExperimental) ||
        (s.pushflowTier != PushFlowTier::Fast && s.pushflowTier != PushFlowTier::Quality) ||
        !wmemchr(s.pushflowRuntimeDir, L'\0', ARRAYSIZE(s.pushflowRuntimeDir)) ||
        !wmemchr(s.anrDllPath, L'\0', ARRAYSIZE(s.anrDllPath))) return false;
    return s.backend != Backend::AnrExperimental || (s.experimentalAnrOptIn == 1 && s.anrDllPath[0]);
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
