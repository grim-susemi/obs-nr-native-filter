// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_ipc.h"

namespace nr::fg::vfg {
inline constexpr uint32_t max_multiplier = 4;
enum class Quality : uint32_t { Low = 0, Medium = 1, High = 2 };
inline constexpr Quality decode_quality(long long mode) noexcept
{
    return mode == 0 ? Quality::Low : mode == 2 ? Quality::High : Quality::Medium;
}

struct Session {
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
};
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
