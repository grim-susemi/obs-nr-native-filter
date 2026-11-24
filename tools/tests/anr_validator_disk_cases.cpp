// SPDX-License-Identifier: GPL-3.0-only
#include "fg/anr_dll_validator.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using nr::fg::anr::ValidationStatus;
namespace fs = std::filesystem;

constexpr std::string_view pack = "programs/fg-lite-any-dp4a-srgb8/pack.nrx";
constexpr std::string_view companions[]{
    "nr_fg.ini", "path_table.json", "size_table.json",
    "adapters/pack.nrx", "kernels/pack.nrx", "kernels-portable/pack.nrx",
    "programs/fg-lite-any-coopmat-pq10/pack.nrx", "programs/fg-lite-any-coopmat-srgb8/pack.nrx",
    "programs/fg-lite-any-dp4a-pq10/pack.nrx", pack,
    "programs/fg-lite-any-packed-pq10/pack.nrx", "programs/fg-lite-any-packed-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-linalg-pq10/pack.nrx", "programs/fg-lite-any-portable-linalg-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-pq10/pack.nrx", "programs/fg-lite-any-portable-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-xmx-pq10/pack.nrx", "programs/fg-lite-any-portable-xmx-srgb8/pack.nrx",
};

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Scratch {
    const fs::path dir;
    ~Scratch()
    {
        if (dir.empty()) return;
        std::error_code error;
        fs::remove_all(dir, error);
        if (error) std::cerr << "scratch cleanup failed: " << error.message() << '\n';
    }
};

void load_pinned_fixture(const nr::fg::anr::PinnedAddon& pinned)
{
    const auto module = LoadLibraryExW(pinned.canonicalPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "fixture load");
    struct Loaded {
        HMODULE module;
        ~Loaded() { FreeLibrary(module); }
    } loaded{module};
    require(GetProcAddress(loaded.module, "NVSDK_NGX_D3D12_Init_Ext") != nullptr &&
        GetProcAddress(loaded.module, "NVSDK_NGX_D3D12_Shutdown1") != nullptr,
        "Windows resolved fixture exports");
}
} // namespace

void anr_validator_disk_cases()
{
    wchar_t exe[32768]{};
    const auto length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    require(length && length < ARRAYSIZE(exe), "resolve fixture executable");
    const auto binary = fs::path(exe).parent_path() / L"anr_benign_fixture.dll";
    const auto owned = fs::current_path() / (L"anr-disk-cases-" + std::to_wstring(GetCurrentProcessId()));
    require(fs::create_directory(owned), "create owned fixture root");
    const Scratch scratch{owned};
    const auto path = scratch.dir / L"nvngx_dlssg.dll";
    fs::copy_file(binary, path);

    const auto exercise = [&](std::string_view expectedMissing) {
        const auto validation = nr::fg::anr::validate_addon(path.native(), true);
        if (validation.status == ValidationStatus::Accepted) {
            require(validation.addon != nullptr, "accepted token");
            const auto& pinned = *validation.addon;
            const auto requested = std::wstring(LR"(\\?\)") + path.native();
            require(pinned.canonicalPath == requested && pinned.sha256.size() == 64, "pinned DLL identity and SHA-256");
            const auto verifyPin = [&](const fs::path& file) {
                const auto expected = std::wstring(LR"(\\?\)") + file.native();
                require(std::ranges::any_of(pinned.entries, [&](const auto& entry) {
                    return !entry->isDirectory && entry->finalPath == expected;
                }), "every required file retains its own pin");
            };
            verifyPin(path);
            for (auto relative : companions) verifyPin(scratch.dir / fs::path(relative).make_preferred());
            load_pinned_fixture(pinned);
            std::cout << "fixture_sha256=" << pinned.sha256 << " pinned_files=19\n";
        }
        const auto expected = expectedMissing.empty() ? ValidationStatus::Accepted :
            ValidationStatus::IncompleteAddonMissingCompanions;
        if (validation.status != expected || validation.relativePath != expectedMissing) {
            std::cerr << "unexpected fixture result: status=" << nr::fg::anr::status_name(validation.status)
                << " relative_path=" << validation.relativePath << '\n';
            throw std::runtime_error("validator/loader boundary result");
        }
        std::cout << "disk status=" << nr::fg::anr::status_name(validation.status)
            << " relative_path=" << validation.relativePath << '\n';
    };
    require(nr::fg::anr::validate_addon(path.native(), false).status == ValidationStatus::OptInRequired,
        "real fixture opt-in gate");
    exercise("nr_fg.ini");
    for (auto relative : companions) {
        const auto file = scratch.dir / fs::path(relative);
        fs::create_directories(file.parent_path());
        std::ofstream ordinary;
        ordinary.exceptions(std::ios::failbit | std::ios::badbit);
        ordinary.open(file, std::ios::binary);
        ordinary << "owned anr test companion\n";
        ordinary.close();
    }
    exercise({});
    require(fs::remove(scratch.dir / fs::path(pack)), "remove one owned model pack");
    exercise(pack);
}
