// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nr::fg::anr {
enum class ValidationStatus {
    Accepted, OptInRequired, InvalidPath, IoFailure, InvalidSize, InvalidPe,
    MissingExport, InvalidExport, ReparseOrRootEscape, IncompleteAddonMissingCompanions, HashFailure,
};

inline constexpr std::string_view b15Sha256 =
    "ae7424b87d13a0e3a9a99dd02d1ea35b9de96c2caa9cd4300d73bac5bb0cf4e8";

enum class EntryKind { File, Directory };
enum class OpenStatus { Opened, Missing, Failed };

struct OpenedEntry {
    virtual ~OpenedEntry() = default;
    virtual bool read(std::span<unsigned char> bytes) = 0;
    std::wstring finalPath;
    uint64_t size{};
    bool isDirectory{};
    bool isReparsePoint{};
};

struct OpenResult {
    OpenStatus status;
    std::unique_ptr<OpenedEntry> entry;
};

struct FileSource {
    virtual ~FileSource() = default;
    virtual OpenResult open(const std::wstring& path, EntryKind kind) const = 0;
};

// Production file boundary: fixed, removable or RAM disks only; every handle is
// opened with FILE_FLAG_OPEN_REPARSE_POINT and must resolve to its own path.
struct DiskSource final : FileSource {
    OpenResult open(const std::wstring& path, EntryKind kind) const override;
};

struct PinnedAddon {
    std::wstring canonicalPath;
    std::string sha256;
    // Retain this object through use of canonicalPath, including any future load.
    std::vector<std::unique_ptr<OpenedEntry>> entries;
};

struct Validation {
    ValidationStatus status;
    std::string_view relativePath{};
    std::unique_ptr<PinnedAddon> addon{};
};

const char* status_name(ValidationStatus status) noexcept;
// Generic x64 PE/DLL export inspection, shared with the XeMFG provider gate.
ValidationStatus inspect_exports(std::span<const unsigned char> bytes,
    std::span<const std::string_view> names);
ValidationStatus inspect_exports(std::span<const unsigned char> bytes);
Validation validate_addon(std::wstring_view path, bool experimentalOptIn, const FileSource& files);
Validation validate_addon(std::wstring_view path, bool experimentalOptIn);
} // namespace nr::fg::anr
