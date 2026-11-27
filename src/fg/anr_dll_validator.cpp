// SPDX-License-Identifier: GPL-3.0-only
#include "anr_dll_validator.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace nr::fg::anr {
namespace {
using Status = ValidationStatus;
using Bytes = std::span<const unsigned char>;
constexpr std::wstring_view extended = LR"(\\?\)";
constexpr std::string_view companions[]{
    "nr_fg.ini", "path_table.json", "size_table.json",
    "adapters/pack.nrx", "kernels/pack.nrx", "kernels-portable/pack.nrx",
    "programs/fg-lite-any-coopmat-pq10/pack.nrx", "programs/fg-lite-any-coopmat-srgb8/pack.nrx",
    "programs/fg-lite-any-dp4a-pq10/pack.nrx", "programs/fg-lite-any-dp4a-srgb8/pack.nrx",
    "programs/fg-lite-any-packed-pq10/pack.nrx", "programs/fg-lite-any-packed-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-linalg-pq10/pack.nrx", "programs/fg-lite-any-portable-linalg-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-pq10/pack.nrx", "programs/fg-lite-any-portable-srgb8/pack.nrx",
    "programs/fg-lite-any-portable-xmx-pq10/pack.nrx", "programs/fg-lite-any-portable-xmx-srgb8/pack.nrx",
};
constexpr std::string_view exports[]{
    "NVSDK_NGX_D3D12_Init_Ext", "NVSDK_NGX_D3D12_PopulateParameters_Impl", "NVSDK_NGX_D3D12_CreateFeature",
    "NVSDK_NGX_D3D12_EvaluateFeature", "NVSDK_NGX_D3D12_ReleaseFeature", "NVSDK_NGX_D3D12_Shutdown1",
};
bool equal_path(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool valid_path(std::wstring_view path)
{
    if (path.size() < 4 || path.size() > 32760 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))) return false;
    for (size_t start = 3; start < path.size();) {
        const auto end = path.find(L'\\', start);
        const auto part = path.substr(start, end == path.npos ? path.npos : end - start);
        if (part.empty() || part.back() == L'.' || part.back() == L' ' ||
            std::ranges::any_of(part, [](wchar_t c) { return c < 32 || c == 127 ||
                std::wstring_view(L"\"/:*?<>|").find(c) != std::wstring_view::npos; })) return false;
        if (end == path.npos) return equal_path(part, L"nvngx_dlssg.dll");
        start = end + 1;
    }
    return false;
}
struct DiskEntry final : OpenedEntry {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~DiskEntry() override { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    bool read(std::span<unsigned char> bytes) override
    {
        DWORD got{};
        return ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size();
    }
};
OpenResult open_disk(const std::wstring& path, EntryKind kind)
{
        wchar_t root[]{path[4], L':', L'\\', L'\0'};
        const auto driveType = GetDriveTypeW(root);
        if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE && driveType != DRIVE_RAMDISK)
            return {OpenStatus::Failed, {}};
        auto entry = std::make_unique<DiskEntry>();
        // A directory pin needs data access so Windows enforces the sharing mode.
        entry->handle = CreateFileW(path.c_str(), kind == EntryKind::Directory ?
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (entry->handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return {error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
                OpenStatus::Missing : OpenStatus::Failed, {}};
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(entry->handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(entry->handle, &info))
            return {OpenStatus::Failed, {}};
        entry->isDirectory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry->isReparsePoint = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry->size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (!entry->isReparsePoint) {
            entry->finalPath.resize(32768);
            const auto n = GetFinalPathNameByHandleW(entry->handle, entry->finalPath.data(),
                static_cast<DWORD>(entry->finalPath.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (!n || n >= entry->finalPath.size()) return {OpenStatus::Failed, {}};
            entry->finalPath.resize(n);
        }
        return {OpenStatus::Opened, std::move(entry)};
}
template<class T> bool read(Bytes bytes, size_t offset, T& out)
{
    if (offset > bytes.size() || sizeof(out) > bytes.size() - offset) return false;
    std::memcpy(&out, bytes.data() + offset, sizeof(out));
    return true;
}
} // namespace

OpenResult DiskSource::open(const std::wstring& path, EntryKind kind) const
{
    return open_disk(path, kind);
}

ValidationStatus inspect_exports(Bytes bytes, std::span<const std::string_view> wanted)
{
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        !read(bytes, static_cast<size_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(nt.OptionalHeader) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes != IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96) return Status::InvalidPe;
    const auto base = static_cast<size_t>(dos.e_lfanew) + sizeof(nt);
    std::array<IMAGE_SECTION_HEADER, 96> sections{};
    for (size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        auto& s = sections[i];
        if (!read(bytes, base + i * sizeof(s), s) || s.VirtualAddress < nt.OptionalHeader.SizeOfHeaders ||
            (s.SizeOfRawData && s.PointerToRawData < nt.OptionalHeader.SizeOfHeaders) ||
            uint64_t(s.PointerToRawData) + s.SizeOfRawData > bytes.size() ||
            uint64_t(s.VirtualAddress) + std::max(s.SizeOfRawData, s.Misc.VirtualSize) > nt.OptionalHeader.SizeOfImage)
            return Status::InvalidPe;
        for (const auto& p : std::span(sections).first(i)) {
            if (uint64_t(s.VirtualAddress) < uint64_t(p.VirtualAddress) + std::max(p.SizeOfRawData, p.Misc.VirtualSize) &&
                uint64_t(p.VirtualAddress) < uint64_t(s.VirtualAddress) + std::max(s.SizeOfRawData, s.Misc.VirtualSize))
                return Status::InvalidPe;
        }
    }
    if (base + nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) > nt.OptionalHeader.SizeOfHeaders)
        return Status::InvalidPe;
    const auto region = [&](DWORD rva, bool executable = false) -> Bytes {
        for (const auto& s : std::span(sections).first(nt.FileHeader.NumberOfSections)) {
            if (rva < s.VirtualAddress || (executable && !(s.Characteristics & IMAGE_SCN_MEM_EXECUTE))) continue;
            const auto delta = rva - s.VirtualAddress;
            const auto length = std::min(s.SizeOfRawData, s.Misc.VirtualSize);
            if (delta < length) return bytes.subspan(static_cast<size_t>(s.PointerToRawData) + delta, length - delta);
        }
        return {};
    };
    const auto dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto directory = region(dir.VirtualAddress);
    IMAGE_EXPORT_DIRECTORY table{};
    if (dir.Size < sizeof(table) || dir.Size > directory.size() || !read(directory, 0, table)) return Status::InvalidPe;
    const auto nameTable = region(table.AddressOfNames), ordinals = region(table.AddressOfNameOrdinals), functions = region(table.AddressOfFunctions);
    if (table.NumberOfNames > nameTable.size() / sizeof(DWORD) || table.NumberOfNames > ordinals.size() / sizeof(WORD) ||
        table.NumberOfFunctions > functions.size() / sizeof(DWORD)) return Status::InvalidPe;
    std::vector<bool> found(wanted.size());
    std::string_view previous;
    for (size_t i = 0; i < table.NumberOfNames; ++i) {
        DWORD name{}, fn{};
        WORD ordinal{};
        if (!read(nameTable, i * sizeof(name), name) || !read(ordinals, i * sizeof(ordinal), ordinal) ||
            ordinal >= table.NumberOfFunctions || !read(functions, ordinal * sizeof(fn), fn)) return Status::InvalidPe;
        const auto nameBytes = region(name);
        if (nameBytes.empty()) return Status::InvalidPe;
        const auto chars = reinterpret_cast<const char*>(nameBytes.data());
        const auto terminator = static_cast<const char*>(std::memchr(chars, 0, std::min<size_t>(256, nameBytes.size())));
        if (!terminator || terminator == chars) return Status::InvalidPe;
        const std::string_view key(chars, static_cast<size_t>(terminator - chars));
        if (!previous.empty() && previous >= key) return Status::InvalidPe;
        previous = key;
        const auto match = std::ranges::find(wanted, key);
        if (match == wanted.end()) continue;
        if ((fn >= dir.VirtualAddress && fn - dir.VirtualAddress < dir.Size) || region(fn, true).empty())
            return Status::InvalidExport;
        found[static_cast<size_t>(match - wanted.begin())] = true;
    }
    return std::ranges::all_of(found, [](bool value) { return value; }) ? Status::Accepted : Status::MissingExport;
}

ValidationStatus inspect_exports(Bytes bytes)
{
    return inspect_exports(bytes, exports);
}

const char* status_name(Status status) noexcept
{
    switch (status) {
    case Status::Accepted: return "accepted";
    case Status::OptInRequired: return "experimental_opt_in_required";
    case Status::InvalidPath: return "invalid_anr_dll_path";
    case Status::IoFailure: return "anr_io_failure";
    case Status::InvalidSize: return "invalid_anr_dll_size";
    case Status::InvalidPe: return "invalid_anr_pe";
    case Status::MissingExport: return "missing_anr_export";
    case Status::InvalidExport: return "forwarded_or_invalid_anr_export";
    case Status::ReparseOrRootEscape: return "reparse_or_root_escape";
    case Status::IncompleteAddonMissingCompanions: return "incomplete_anr_addon_missing_companions";
    case Status::HashFailure: return "anr_sha256_failure";
    }
    return "unknown_anr_validation_status";
}

Validation validate_addon(std::wstring_view path, bool experimentalOptIn, const FileSource& files)
{
    if (!experimentalOptIn) return {Status::OptInRequired};
    if (path.starts_with(extended)) path.remove_prefix(extended.size());
    if (!valid_path(path)) return {Status::InvalidPath};
    const auto expected = std::wstring(extended) + std::wstring(path);
    auto addon = std::make_unique<PinnedAddon>();
    const auto pin = [&](const std::wstring& expectedPath, EntryKind kind, std::string_view companion) {
        auto opened = files.open(expectedPath, kind);
        const auto missing = companion.empty() ? Status::IoFailure : Status::IncompleteAddonMissingCompanions;
        if (opened.status == OpenStatus::Missing) return missing;
        if (opened.status != OpenStatus::Opened || !opened.entry) return Status::IoFailure;
        const auto& entry = *opened.entry;
        if (entry.isReparsePoint || !equal_path(expectedPath, entry.finalPath)) return Status::ReparseOrRootEscape;
        if (entry.isDirectory != (kind == EntryKind::Directory)) return Status::IoFailure;
        if (!companion.empty() && kind == EntryKind::File && entry.size == 0) return missing;
        addon->entries.push_back(std::move(opened.entry));
        return Status::Accepted;
    };
    // Pin each ancestor before the DLL; keep all validated path components and files open.
    for (auto pos = expected.find(L'\\', 7); pos != expected.npos; pos = expected.find(L'\\', pos + 1)) {
        if (const auto status = pin(expected.substr(0, pos), EntryKind::Directory, {}); status != Status::Accepted)
            return {status};
    }
    if (const auto status = pin(expected, EntryKind::File, {}); status != Status::Accepted) return {status};
    auto& dll = *addon->entries.back();
    if (dll.size < 64 * 1024 || dll.size > 64 * 1024 * 1024) return {Status::InvalidSize};
    addon->canonicalPath = dll.finalPath;
    std::vector<unsigned char> bytes(static_cast<size_t>(dll.size));
    if (!dll.read(bytes)) return {Status::IoFailure};
    if (const auto status = inspect_exports(bytes); status != Status::Accepted) return {status};
    const auto root = dll.finalPath.substr(0, dll.finalPath.rfind(L'\\') + 1);
    std::string_view firstMissing;
    for (const auto relative : companions) {
        std::wstring target = root;
        auto status = Status::Accepted;
        for (const auto c : relative) {
            if (c == '/') {
                status = pin(target, EntryKind::Directory, relative);
                if (status != Status::Accepted) break;
            }
            target += c == '/' ? L'\\' : static_cast<wchar_t>(c);
        }
        if (status == Status::Accepted) status = pin(target, EntryKind::File, relative);
        if (status == Status::IncompleteAddonMissingCompanions) {
            if (firstMissing.empty()) firstMissing = relative;
        } else if (status != Status::Accepted) return {status, relative};
    }
    if (!firstMissing.empty()) return {Status::IncompleteAddonMissingCompanions, firstMissing};
    std::array<unsigned char, 32> hash{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
        hash.data(), static_cast<ULONG>(hash.size())) < 0) return {Status::HashFailure};
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : hash) {
        addon->sha256 += hex[byte >> 4];
        addon->sha256 += hex[byte & 15];
    }
    return {Status::Accepted, {}, std::move(addon)};
}

Validation validate_addon(std::wstring_view path, bool experimentalOptIn)
{
    return validate_addon(path, experimentalOptIn, DiskSource{});
}
} // namespace nr::fg::anr
