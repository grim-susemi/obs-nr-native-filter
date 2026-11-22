// SPDX-License-Identifier: GPL-3.0-only
#include "fg/anr_dll_validator.h"
#include "anr_ngx_keys.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace nr::fg::anr;
using Status = ValidationStatus;
namespace fs = std::filesystem;
constexpr std::wstring_view root = LR"(\\?\C:\anr-fixture\)";
constexpr std::wstring_view dll = LR"(C:\anr-fixture\nvngx_dlssg.dll)";
constexpr std::wstring_view missingPack = LR"(programs\fg-lite-any-dp4a-srgb8\pack.nrx)";
constexpr std::string_view missingName = "programs/fg-lite-any-dp4a-srgb8/pack.nrx";

enum class ParserCase { Complete, MissingExport, ForwardedExport };

// Metadata only. This buffer contains no executable code and is never written or loaded.
std::vector<unsigned char> export_metadata(ParserCase scenario = ParserCase::Complete)
{
    constexpr std::string_view names[]{
        "NVSDK_NGX_D3D12_CreateFeature", "NVSDK_NGX_D3D12_EvaluateFeature", "NVSDK_NGX_D3D12_Init_Ext",
        "NVSDK_NGX_D3D12_PopulateParameters_Impl", "NVSDK_NGX_D3D12_ReleaseFeature", "NVSDK_NGX_D3D12_Shutdown1",
    };
    std::vector<unsigned char> bytes(65536);
    const auto put = [&](size_t offset, const auto& value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    put(0, dos);
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 1;
    nt.FileHeader.Characteristics = IMAGE_FILE_DLL;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfHeaders = 0x200;
    nt.OptionalHeader.SizeOfImage = 0x2000;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {0x1000, 0x300};
    put(0x80, nt);
    IMAGE_SECTION_HEADER section{};
    section.VirtualAddress = 0x1000;
    section.Misc.VirtualSize = 0x800;
    section.SizeOfRawData = 0x800;
    section.PointerToRawData = 0x200;
    section.Characteristics = IMAGE_SCN_MEM_EXECUTE;
    put(0x80 + sizeof(nt), section);
    IMAGE_EXPORT_DIRECTORY table{};
    table.NumberOfNames = scenario == ParserCase::MissingExport ? 5 : 6;
    table.NumberOfFunctions = 6;
    table.AddressOfNames = 0x1040;
    table.AddressOfNameOrdinals = 0x1060;
    table.AddressOfFunctions = 0x1080;
    put(0x200, table);
    DWORD next = 0x1100;
    for (WORD i = 0; i < table.NumberOfFunctions; ++i) {
        put(0x240 + i * sizeof(DWORD), next);
        put(0x260 + i * sizeof(WORD), i);
        const DWORD address = scenario == ParserCase::ForwardedExport && i == 0 ? 0x1200 : 0x1400;
        put(0x280 + i * sizeof(DWORD), address);
        std::ranges::copy(names[i], bytes.begin() + (next - 0x1000 + 0x200));
        next += static_cast<DWORD>(names[i].size() + 1);
    }
    return bytes;
}

struct MockEntry final : OpenedEntry {
    explicit MockEntry(int& count) : count_(count) { ++count_; }
    ~MockEntry() override { --count_; }
    bool read(std::span<unsigned char> into) override
    {
        const auto metadata = export_metadata();
        if (into.size() != metadata.size()) return false;
        std::ranges::copy(metadata, into.begin());
        return true;
    }
    int& count_;
};

struct MockSource final : FileSource {
    MockSource()
    {
        entries.emplace(root.substr(0, root.size() - 1), EntryKind::Directory);
        constexpr std::wstring_view directories[]{
            L"adapters", L"kernels", L"kernels-portable", L"programs",
            LR"(programs\fg-lite-any-coopmat-pq10)", LR"(programs\fg-lite-any-coopmat-srgb8)",
            LR"(programs\fg-lite-any-dp4a-pq10)", LR"(programs\fg-lite-any-dp4a-srgb8)",
            LR"(programs\fg-lite-any-packed-pq10)", LR"(programs\fg-lite-any-packed-srgb8)",
            LR"(programs\fg-lite-any-portable-linalg-pq10)", LR"(programs\fg-lite-any-portable-linalg-srgb8)",
            LR"(programs\fg-lite-any-portable-pq10)", LR"(programs\fg-lite-any-portable-srgb8)",
            LR"(programs\fg-lite-any-portable-xmx-pq10)", LR"(programs\fg-lite-any-portable-xmx-srgb8)",
        };
        for (auto folder : directories) {
            const auto path = std::wstring(root) + std::wstring(folder);
            entries.emplace(path, EntryKind::Directory);
            if (folder != L"programs") entries.emplace(path + LR"(\pack.nrx)", EntryKind::File);
        }
        for (const auto name : {L"nvngx_dlssg.dll", L"nr_fg.ini", L"path_table.json", L"size_table.json"})
            entries.emplace(std::wstring(root) + name, EntryKind::File);
    }
    OpenResult open(const std::wstring& path, EntryKind kind) const override
    {
        ++opens;
        const auto found = entries.find(path);
        if (path == denied) return {OpenStatus::Failed, {}};
        if (found == entries.end()) return {OpenStatus::Missing, {}};
        if (found->second != kind) return {OpenStatus::Failed, {}};
        auto entry = std::make_unique<MockEntry>(live);
        entry->finalPath = path == mismatched ? LR"(\\?\C:\mock-different-identity)" : path;
        entry->isReparsePoint = path == reparse;
        entry->isDirectory = kind == EntryKind::Directory;
        entry->size = path == empty ? 0 : path.ends_with(L"nvngx_dlssg.dll") ? 65536 : 1;
        return {OpenStatus::Opened, std::move(entry)};
    }
    std::map<std::wstring, EntryKind> entries;
    std::wstring denied, mismatched, reparse, empty;
    mutable int live{}, opens{};
};

void check(bool pass, std::string_view label)
{
    if (!pass) throw std::runtime_error(std::string(label));
    std::cout << "pass " << label << '\n';
}

void path_cases()
{
    const MockSource source;
    check(validate_addon(dll, false, source).status == Status::OptInRequired, "opt-in disabled");
    for (const auto path : {L"", LR"(\\server\share\nvngx_dlssg.dll)", LR"(C:\anr-fixture\wrong.dll)",
        LR"(anr-fixture\nvngx_dlssg.dll)", LR"("C:\anr-fixture\nvngx_dlssg.dll")",
        L"C:\\anr-fixture\t\\nvngx_dlssg.dll"}) {
        check(validate_addon(path, true, source).status == Status::InvalidPath, "invalid path syntax");
    }
    check(source.opens == 0, "option and syntax rejection precede file access");
}

void addon_cases()
{
    MockSource source;
    const auto verify = [&](Status status, std::string_view label, std::string_view path = {}) {
        const auto result = validate_addon(dll, true, source);
        check(result.status == status && result.relativePath == path && !result.addon && source.live == 0, label);
    };
    const auto pinnedPath = std::wstring(root) + L"nvngx_dlssg.dll";
    source.reparse = pinnedPath;
    verify(Status::ReparseOrRootEscape, "DLL reparse metadata");
    source.reparse.clear();
    source.mismatched = pinnedPath;
    verify(Status::ReparseOrRootEscape, "DLL final path mismatch");
    source.mismatched.clear();
    source.denied = pinnedPath;
    verify(Status::IoFailure, "DLL access denied");
    source.denied.clear();
    {
        const auto result = validate_addon(dll, true, source);
        check(result.status == Status::Accepted && result.addon && result.addon->canonicalPath == pinnedPath &&
            result.addon->sha256 == "3d18e207e80ce38c5d8e7d138e1e694f0c91549a70bbfdf6565b581ace6157ca" &&
            source.live >= 19, "complete mock addon returns pinned identity and digest");
        std::cout << "fixture_sha256=" << result.addon->sha256 << '\n';
    }
    check(source.live == 0, "releasing accepted result closes pins");
    source.reparse = std::wstring(root) + L"programs";
    verify(Status::ReparseOrRootEscape, "intermediate reparse metadata", "programs/fg-lite-any-coopmat-pq10/pack.nrx");
    source.reparse.clear();
    const auto pack = std::wstring(root) + std::wstring(missingPack);
    source.mismatched = pack;
    verify(Status::ReparseOrRootEscape, "companion final path mismatch", missingName);
    source.mismatched.clear();
    source.denied = pack;
    verify(Status::IoFailure, "companion access denied", missingName);
    source.denied.clear();
    source.empty = pack;
    verify(Status::IncompleteAddonMissingCompanions, "empty companion", missingName);
    source.empty.clear();
    source.entries.erase(pack);
    verify(Status::IncompleteAddonMissingCompanions, "18-of-19 addon", missingName);
    source.denied = std::wstring(root) + LR"(programs\fg-lite-any-portable-xmx-srgb8\pack.nrx)";
    verify(Status::IoFailure, "missing earlier companion does not mask later denied access",
        "programs/fg-lite-any-portable-xmx-srgb8/pack.nrx");
    source.denied.clear();
    std::erase_if(source.entries, [&](const auto& entry) {
        return entry.first != pinnedPath && entry.first != root.substr(0, root.size() - 1);
    });
    verify(Status::IncompleteAddonMissingCompanions, "standalone DLL", "nr_fg.ini");
}

void disk_case()
{
    const auto dir = fs::current_path() / (L"anr-validator-test-" + std::to_wstring(GetCurrentProcessId()));
    if (!fs::create_directory(dir)) throw std::runtime_error("create owned scratch");
    struct Scratch {
        fs::path dir;
        ~Scratch() { std::error_code error; fs::remove_all(dir, error); }
    } scratch{dir};
    const auto path = dir / L"nvngx_dlssg.dll";
    std::ofstream ordinary(path, std::ios::binary);
    ordinary.exceptions(std::ios::failbit | std::ios::badbit);
    ordinary << std::string(65536, 'x');
    ordinary.close();
    check(validate_addon(path.native(), true).status == Status::InvalidPe, "ordinary local file rejected by PE parser");
}
} // namespace

void anr_validator_disk_cases();

int main()
{
    try {
        path_cases();
        check(inspect_exports(export_metadata(ParserCase::MissingExport)) == Status::MissingExport, "missing exact export");
        check(inspect_exports(export_metadata(ParserCase::ForwardedExport)) == Status::InvalidExport, "forwarded export metadata");
        const auto complete = export_metadata();
        check(inspect_exports(std::span(complete).first(0x100)) == Status::InvalidPe, "truncated metadata");
        addon_cases();
        disk_case();
        anr_validator_disk_cases();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
