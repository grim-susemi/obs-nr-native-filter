// SPDX-License-Identifier: GPL-3.0-only
#include "fg/anr_dll_validator.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using nr::fg::anr::ValidationStatus;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct LinkCase {
    const char* label;
    fs::path entry;
    fs::path target;
    DWORD flags;
    std::string_view expectedPath;
};

bool check_refusal(const LinkCase& test, const fs::path& dll)
{
    if (!CreateSymbolicLinkW(test.entry.c_str(), test.target.c_str(),
            test.flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        const auto error = GetLastError();
        std::cerr << test.label << " not run: CreateSymbolicLinkW error=" << error << '\n';
        return false;
    }
    // Remove the link itself before recursive cleanup of ordinary fixture files.
    struct RemoveLink {
        const LinkCase& test;
        ~RemoveLink()
        {
            if (!removed && !remove())
                std::cerr << test.label << " link cleanup error=" << GetLastError() << '\n';
        }
        bool remove()
        {
            removed = test.flags == SYMBOLIC_LINK_FLAG_DIRECTORY ?
                RemoveDirectoryW(test.entry.c_str()) != FALSE : DeleteFileW(test.entry.c_str()) != FALSE;
            return removed;
        }
        bool removed{};
    } link{test};
    const auto attributes = GetFileAttributesW(test.entry.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT),
        "fixture does not have the reparse attribute");
    const auto validation = nr::fg::anr::validate_addon(dll.native(), true);
    std::cout << test.label << " status=" << nr::fg::anr::status_name(validation.status)
        << " relative_path=" << validation.relativePath << " reparse_attribute=1\n";
    require(link.remove(), "remove owned link without following it");
    return validation.status == ValidationStatus::ReparseOrRootEscape && validation.relativePath == test.expectedPath &&
        !validation.addon;
}
} // namespace

bool anr_controller_junction_case(const std::filesystem::path& root);

int wmain(int argc, wchar_t** argv)
{
    try {
        require(argc == 3, "expected benign DLL and owned scratch prefix");
        auto root = fs::path(argv[2]);
        root += L"-" + std::to_wstring(GetCurrentProcessId());
        root.make_preferred();
        require(fs::create_directory(root), "create owned scratch");
        const auto addon = root / L"addon";
        const auto sibling = root / L"ordinary-target";
        const auto dll = addon / L"nvngx_dlssg.dll";
        const auto intermediate = addon / L"adapters";
        bool fileRefused{}, directoryRefused{}, junctionRefused{};
        try {
            fs::create_directory(addon);
            fs::create_directory(sibling);
            const auto inertDll = sibling / L"benign.dll";
            fs::copy_file(argv[1], inertDll);
            fileRefused = check_refusal({"file-link", dll, inertDll, 0, {}}, dll);

            fs::copy_file(inertDll, dll);
            std::ofstream ordinary(sibling / L"pack.nrx", std::ios::binary);
            ordinary.exceptions(std::ios::failbit | std::ios::badbit);
            ordinary << "ordinary test companion\n";
            ordinary.close();
            directoryRefused = check_refusal({"intermediate-directory-link", intermediate, sibling,
                SYMBOLIC_LINK_FLAG_DIRECTORY, "adapters/pack.nrx"}, dll);
            junctionRefused = anr_controller_junction_case(root);
        } catch (const std::exception& error) {
            std::cerr << "fixture error: " << error.what() << '\n';
        }
        for (const auto& entry : {dll, intermediate, addon / L"programs" / L"fg-lite-any-dp4a-srgb8"}) {
            const auto attrs = GetFileAttributesW(entry.c_str());
            require(attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_REPARSE_POINT),
                "link remains; withholding recursive cleanup");
        }
        fs::remove_all(root);
        std::cout << "owned scratch removed; file_refused=" << fileRefused
            << " directory_refused=" << directoryRefused << " junction_refused=" << junctionRefused << '\n';
        return fileRefused && directoryRefused && junctionRefused ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
