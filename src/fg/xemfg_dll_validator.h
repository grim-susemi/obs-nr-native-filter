// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Opt-in admission for a user-supplied MFGUnlocker.dll and the official Intel
// XeSS companion pair. The community provider is never Authenticode-checked
// (valid builds are unsigned); the Intel libraries keep the strict
// Intel-Corporation signature and pinned-hash gate from xefg_runtime.
#include "anr_dll_validator.h"
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nr::fg::xemfg {
enum class ValidationStatus {
    Accepted,
    OptInRequired,
    InvalidPath,
    IoFailure,
    InvalidSize,
    InvalidPe,
    MissingExport,
    InvalidExport,
    ReparseOrRootEscape,
    CompanionRejected,
    HashFailure,
};

const char* status_name(ValidationStatus status) noexcept;

inline constexpr std::string_view provider_export = "MfgXeMFGProviderGetApi";
inline constexpr std::wstring_view companion_directory = L"RTXMFG-XeSS";

// Owns the admitted Intel images for as long as the provider may call them.
// The concrete implementation lives beside the presenter's XeFgRuntime.
struct CompanionLease {
    virtual ~CompanionLease() = default;
    std::wstring directory;
};

struct CompanionResult {
    ValidationStatus status{ValidationStatus::CompanionRejected};
    std::string detail;
    std::unique_ptr<CompanionLease> lease;
};

struct CompanionValidator {
    virtual ~CompanionValidator() = default;
    // Candidates are already ordered by preference. Accepted must return a lease.
    virtual CompanionResult validate(std::span<const std::wstring> candidates) = 0;
};

struct PinnedProvider {
    std::wstring canonicalPath;
    std::string sha256;
    std::wstring companionDirectory;
    std::unique_ptr<CompanionLease> companions;
    // Ancestor directories first, then the provider image. Keeping these open
    // means the admitted bytes and the loaded image cannot be swapped.
    std::vector<std::unique_ptr<anr::OpenedEntry>> entries;
};

struct Validation {
    ValidationStatus status{ValidationStatus::OptInRequired};
    // Stable reason for the log: the refused component or the companion gate.
    std::wstring detail;
    std::unique_ptr<PinnedProvider> provider;
};

// `xessRuntimeDir` is tried first when non-empty; <provider dir>/RTXMFG-XeSS is
// always the fallback candidate. The directory must contain libxess_fg.dll
// 1.3.1.78 and libxell.dll 1.3.2.10 with Intel's signature and the pinned hash.
Validation validate_provider(std::wstring_view path, bool experimentalOptIn,
    std::wstring_view xessRuntimeDir, const anr::FileSource& files,
    CompanionValidator& companions);
Validation validate_provider(std::wstring_view path, bool experimentalOptIn,
    std::wstring_view xessRuntimeDir, CompanionValidator& companions);
} // namespace nr::fg::xemfg
