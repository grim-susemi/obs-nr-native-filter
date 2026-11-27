// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include "vfg_trace.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
using namespace nr::fg::vfg;
namespace fs = std::filesystem;

void expect(bool ok, const char* reason)
{
    if (!ok) throw std::runtime_error(reason);
}

struct SavedEnv {
    const wchar_t* name;
    std::wstring value;
    bool present{};
    explicit SavedEnv(const wchar_t* key) : name(key)
    {
        wchar_t buf[4096]{};
        const DWORD n = GetEnvironmentVariableW(key, buf, ARRAYSIZE(buf));
        present = n > 0 && n < ARRAYSIZE(buf);
        if (present) value = buf;
    }
    ~SavedEnv() { SetEnvironmentVariableW(name, present ? value.c_str() : nullptr); }
    SavedEnv(const SavedEnv&) = delete;
    SavedEnv& operator=(const SavedEnv&) = delete;
};

HANDLE create_log(const fs::path& path)
{
    return CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
}

void policy_contract(const fs::path& scratch)
{
    SavedEnv logEnv(L"OBS_NR_VFG_LOG");
    SavedEnv traceEnv(L"OBS_NR_VFG_TRACE");
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", nullptr);
    SetEnvironmentVariableW(L"OBS_NR_VFG_TRACE", nullptr);
    auto config = trace_config_from_env();
    expect(!config.enabled && !config.explicitLog && config.limitBytes == (2LL << 20),
        "real usage leaves frame trace off behind a 2 MiB safety cap");
    SetEnvironmentVariableW(L"OBS_NR_VFG_TRACE", L"1");
    config = trace_config_from_env();
    expect(config.enabled && !config.explicitLog && config.limitBytes == (2LL << 20),
        "OBS_NR_VFG_TRACE=1 enables capped frame trace");
    SetEnvironmentVariableW(L"OBS_NR_VFG_TRACE", L"0");
    config = trace_config_from_env();
    expect(!config.enabled, "OBS_NR_VFG_TRACE=0 keeps frame trace off");
    SetEnvironmentVariableW(L"OBS_NR_VFG_TRACE", nullptr);
    const auto log = (scratch / L"policy.log").wstring();
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    config = trace_config_from_env();
    expect(config.enabled && config.explicitLog && config.limitBytes == 0,
        "an explicit test log path keeps the unlimited full trace");
}

void quiet_frame_sink(const fs::path& scratch)
{
    const auto path = scratch / L"quiet.log";
    const HANDLE file = create_log(path);
    expect(file != INVALID_HANDLE_VALUE, "create quiet log");
    TraceSink sink;
    sink.file = file;
    sink.enabled = false;
    sink.limitBytes = 2LL << 20;
    char line[256];
    const int len = snprintf(line, sizeof(line),
        "event=present_vfg_subframe generation=1 b=%llu b_qpc_ns=%llu a=%llu present_id=1\n",
        123ULL, 456ULL, 789ULL);
    expect(len > 0, "format a frame trace line");
    for (int i = 0; i < 200000; ++i) expect(!sink.send(line, len), "disabled frame trace must not write");
    expect(sink.writtenBytes == 0, "disabled frame trace must not count bytes");
    const char* errorLine = "event=error message=backend_fault\n";
    expect(sink.send(errorLine, static_cast<int>(std::strlen(errorLine)), true),
        "a forced diagnostic still writes while frame trace is off");
    CloseHandle(file);
    expect(fs::file_size(path) == std::strlen(errorLine),
        "200k real-usage frame lines leave only the forced diagnostic on disk");
}

void budgeted_frame_sink(const fs::path& scratch)
{
    const auto path = scratch / L"capped.log";
    const HANDLE file = create_log(path);
    expect(file != INVALID_HANDLE_VALUE, "create capped log");
    TraceSink sink;
    sink.file = file;
    sink.enabled = true;
    sink.limitBytes = 4096;
    char line[128];
    const int len = snprintf(line, sizeof(line), "event=pair_admitted b=%llu bank=%u\n", 1ULL, 0u);
    expect(len > 0, "format a capped trace line");
    int accepted = 0;
    for (int i = 0; i < 10000; ++i) {
        if (sink.send(line, len)) ++accepted;
    }
    LARGE_INTEGER size{};
    expect(GetFileSizeEx(file, &size) != FALSE, "query capped log size");
    CloseHandle(file);
    expect(accepted > 0 && accepted < 10000, "capped sink stops accepting frame lines");
    expect(size.QuadPart >= 4096 && size.QuadPart <= 4096 + len,
        "capped sink stays within one line of its byte budget");
}

void default_log_truncation(const fs::path& scratch)
{
    SavedEnv logEnv(L"OBS_NR_VFG_LOG");
    SavedEnv tempEnv(L"TEMP");
    SavedEnv tmpEnv(L"TMP");
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", nullptr);
    const auto temp = fs::absolute(scratch / L"temp");
    fs::create_directories(temp);
    SetEnvironmentVariableW(L"TEMP", temp.c_str());
    SetEnvironmentVariableW(L"TMP", temp.c_str());
    const auto log = temp / L"obs-nr-vfg-presenter.log";
    {
        std::ofstream out(log, std::ios::binary | std::ios::trunc);
        out << std::string(8u << 20, 'x');
        expect(out.good(), "seed an oversized default log");
    }
    const auto resolved = detail::resolve_log_path();
    expect(fs::equivalent(resolved, log), "the default log resolves under %TEMP%");
    expect(fs::file_size(log) == 0, "a stale multi-MB default log is truncated at first real-usage resolve");
    {
        std::ofstream out(log, std::ios::binary | std::ios::app);
        out << "event=session_started\n";
        expect(out.good(), "append after the reset");
    }
    expect(detail::resolve_log_path() == resolved, "the default log path stays stable");
    expect(fs::file_size(log) == 22, "truncation happens once per process, not on every resolve");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        expect(argc == 2, "arguments: scratch");
        const fs::path scratch = fs::path(argv[1]) / std::to_wstring(GetCurrentProcessId());
        fs::create_directories(scratch);
        policy_contract(scratch);
        quiet_frame_sink(scratch);
        budgeted_frame_sink(scratch);
        default_log_truncation(scratch);
        std::puts("PASS vfg_log_policy: real-usage frame trace is a no-op, diagnostic tracing is capped, "
            "and the stale %TEMP% log is truncated once per process");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_log_policy: %s\n", e.what());
        return 1;
    }
}
