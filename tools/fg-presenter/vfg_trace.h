// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <cstdio>
#include <windows.h>

namespace nr::fg::vfg {

// Frame tracing is opt-in. Real usage leaves both environment knobs unset, so
// record()/trace() return before formatting and never reach WriteFile.
//   OBS_NR_VFG_LOG   : explicit log path (test harnesses) -> full trace, no cap
//   OBS_NR_VFG_TRACE : diagnostic opt-in -> full trace capped at 2 MiB
struct TraceConfig {
    bool explicitLog{};
    bool enabled{};
    int64_t limitBytes{};
};

inline bool trace_env_present(const wchar_t* name) noexcept
{
    wchar_t value[1024]{};
    const DWORD n = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    return n > 0 && n < ARRAYSIZE(value);
}

inline bool trace_env_enabled() noexcept
{
    wchar_t value[8]{};
    const DWORD n = GetEnvironmentVariableW(L"OBS_NR_VFG_TRACE", value, ARRAYSIZE(value));
    return n > 0 && n < ARRAYSIZE(value) && value[0] != L'0';
}

inline TraceConfig trace_config_from_env() noexcept
{
    TraceConfig config;
    config.explicitLog = trace_env_present(L"OBS_NR_VFG_LOG");
    config.enabled = config.explicitLog || trace_env_enabled();
    // Tests assert tokens across whole runs, so their explicit log path keeps
    // the previous unlimited trace. A real diagnostic opt-in is capped so a
    // stuck frame path can never recreate the multi-GB log.
    config.limitBytes = config.explicitLog ? 0 : (2LL << 20);
    return config;
}

// One-line synchronous sink shared by trace() and trace_event(). A disabled
// sink performs no WriteFile at all; force writes keep rare lifecycle/failure
// events even when frame tracing is off.
struct TraceSink {
    HANDLE file{INVALID_HANDLE_VALUE};
    FILE* stream{};
    bool enabled{};
    int64_t limitBytes{};
    int64_t writtenBytes{};

    bool send(const char* data, int len, bool force = false) noexcept
    {
        if (len <= 0 || (!enabled && !force)) return false;
        if (limitBytes > 0 && writtenBytes >= limitBytes) return false;
        DWORD written{};
        if (file != INVALID_HANDLE_VALUE) {
            if (!WriteFile(file, data, static_cast<DWORD>(len), &written, nullptr)) return false;
        } else if (stream) {
            written = static_cast<DWORD>(fwrite(data, 1, static_cast<size_t>(len), stream));
        } else {
            return false;
        }
        writtenBytes += static_cast<int64_t>(written);
        return written != 0;
    }
};

} // namespace nr::fg::vfg
