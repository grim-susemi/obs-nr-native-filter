// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/base.h>
#include <cstdio>
#include <mutex>

// stdout belongs to machine records. Keep OBS diagnostics on their own channel.
inline void nr_host_log(int level, const char* format, va_list args, void*)
{
    static std::mutex gate;
    const std::lock_guard<std::mutex> lock(gate);
    const char* label = level == LOG_DEBUG ? "debug" : level == LOG_INFO ? "info"
        : level == LOG_WARNING ? "warning" : level == LOG_ERROR ? "error" : "log";
    std::fprintf(stderr, "%s: ", label);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}
inline void nr_host_install_logger() { base_set_log_handler(nr_host_log, nullptr); }
