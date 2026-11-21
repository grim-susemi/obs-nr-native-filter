// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdlib>
#include <cstdint>

namespace nr::quality {
// Opt-in: frame-level logs would be too noisy for ordinary OBS sessions.
inline bool trace_frames() noexcept
{
    static const bool enabled = [] {
        char value[2]{};
        size_t size{};
        return getenv_s(&size, value, sizeof(value), "OBS_NR_QUALITY_TRACE") == 0 &&
               size == sizeof(value) && value[0] == '1';
    }();
    return enabled;
}
inline unsigned long long id(uint64_t value) noexcept { return static_cast<unsigned long long>(value); }
}
