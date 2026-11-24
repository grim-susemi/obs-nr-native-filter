// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace anr_test {
namespace fs = std::filesystem;
inline constexpr std::string_view missingPack = "programs/fg-lite-any-dp4a-srgb8/pack.nrx";
inline constexpr std::string_view companions[]{
    "nr_fg.ini", "path_table.json", "size_table.json",
    "adapters/pack.nrx", "kernels/pack.nrx", "kernels-portable/pack.nrx",
    "programs/fg-lite-any-coopmat-pq10/pack.nrx", "programs/fg-lite-any-coopmat-srgb8/pack.nrx",
    "programs/fg-lite-any-dp4a-pq10/pack.nrx", missingPack,
    "programs/fg-lite-any-packed-pq10/pack.nrx", "programs/fg-lite-any-packed-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-linalg-pq10/pack.nrx", "programs/fg-lite-any-portable-linalg-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-pq10/pack.nrx", "programs/fg-lite-any-portable-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-xmx-pq10/pack.nrx", "programs/fg-lite-any-portable-xmx-srgb8/pack.nrx",
};

inline std::wstring environment(const wchar_t* name)
{
    const auto length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    const auto read = GetEnvironmentVariableW(name, value.data(), length);
    require(read && read < length, "fixture environment read");
    value.resize(read);
    return value;
}

struct Environment {
    const wchar_t* name;
    std::wstring old;
    Environment(const wchar_t* key, const wchar_t* value) : name(key), old(environment(key))
    {
        require(SetEnvironmentVariableW(name, value), "fixture environment write");
    }
    ~Environment() { SetEnvironmentVariableW(name, old.empty() ? nullptr : old.c_str()); }
};

struct Scratch {
    const fs::path dir;
    explicit Scratch(const fs::path& parent) : dir(parent / (L"anr-mock-" + std::to_wstring(GetCurrentProcessId())))
    {
        require(fs::create_directory(dir), "create private add-on fixture scratch");
    }
    ~Scratch()
    {
        std::error_code error;
        fs::remove_all(dir, error);
        if (error) std::fprintf(stderr, "fixture cleanup failed: %s\n", error.message().c_str());
    }
};

inline fs::path stage(const fs::path& root, const fs::path& dll, bool complete)
{
    require(fs::create_directory(root), "create addon");
    const auto path = root / L"nvngx_dlssg.dll";
    fs::copy_file(dll, path);
    if (complete) {
        for (const auto relative : companions) {
            auto companion = root / fs::path(relative);
            fs::create_directories(companion.parent_path());
            std::ofstream file(companion, std::ios::binary);
            file << "inert test companion\n";
            require(static_cast<bool>(file), "stage benign companion");
        }
    }
    return path;
}

inline std::string contents(const fs::path& p)
{
    std::ifstream stream(p, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
} // namespace anr_test
