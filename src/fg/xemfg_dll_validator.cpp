// SPDX-License-Identifier: GPL-3.0-only
#include "xemfg_dll_validator.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>

namespace nr::fg::xemfg {
namespace {
using Status = ValidationStatus;
constexpr std::wstring_view extended = LR"(\\?\)";
// The community provider is a small shim next to its CUDA payload, never a
// multi-hundred-megabyte inference engine. Both bounds only reject clearly
// wrong inputs; the pinned transcript and the export table are the real gates.
constexpr uint64_t min_provider_bytes = 16ull * 1024;
constexpr uint64_t max_provider_bytes = 256ull * 1024 * 1024;

bool equal_nocase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool valid_provider_path(std::wstring_view path)
{
    if (path.size() < 5 || path.size() > 32760 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))) return false;
    for (size_t start = 3; start < path.size();) {
        const auto end = path.find(L'\\', start);
        const auto part = path.substr(start, end == path.npos ? path.npos : end - start);
        if (part.empty() || part.back() == L'.' || part.back() == L' ' ||
            std::ranges::any_of(part, [](wchar_t c) { return c < 32 || c == 127 ||
                std::wstring_view(L"\"/:*?<>|").find(c) != std::wstring_view::npos; })) return false;
        if (end == path.npos) {
            const auto dot = part.rfind(L'.');
            return dot != std::wstring_view::npos && dot + 1 < part.size() &&
                equal_nocase(part.substr(dot), L".dll");
        }
        start = end + 1;
    }
    return false;
}

std::string sha256(std::vector<unsigned char>& bytes)
{
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(),
            static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size())) < 0) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

std::wstring normalize(std::wstring_view path)
{
    if (path.starts_with(extended)) path.remove_prefix(extended.size());
    return std::wstring(path);
}
} // namespace

const char* status_name(Status status) noexcept
{
    switch (status) {
    case Status::Accepted: return "accepted";
    case Status::OptInRequired: return "experimental_xemfg_opt_in_required";
    case Status::InvalidPath: return "invalid_xemfg_dll_path";
    case Status::IoFailure: return "xemfg_io_failure";
    case Status::InvalidSize: return "invalid_xemfg_dll_size";
    case Status::InvalidPe: return "invalid_xemfg_pe";
    case Status::MissingExport: return "missing_xemfg_export";
    case Status::InvalidExport: return "forwarded_or_invalid_xemfg_export";
    case Status::ReparseOrRootEscape: return "reparse_or_root_escape";
    case Status::CompanionRejected: return "xemfg_companion_rejected";
    case Status::HashFailure: return "xemfg_sha256_failure";
    }
    return "unknown_xemfg_validation_status";
}

Validation validate_provider(std::wstring_view path, bool experimentalOptIn,
    std::wstring_view xessRuntimeDir, const anr::FileSource& files, CompanionValidator& companions)
{
    if (!experimentalOptIn) return {Status::OptInRequired, L"opt_in"};
    if (path.starts_with(extended)) path.remove_prefix(extended.size());
    if (!valid_provider_path(path)) return {Status::InvalidPath, std::wstring(path)};
    const auto expected = std::wstring(extended) + std::wstring(path);
    auto provider = std::make_unique<PinnedProvider>();
    const auto pin = [&](const std::wstring& expectedPath, anr::EntryKind kind) {
        auto opened = files.open(expectedPath, kind);
        if (opened.status != anr::OpenStatus::Opened || !opened.entry) return Status::IoFailure;
        const auto& entry = *opened.entry;
        if (entry.isReparsePoint || !equal_nocase(expectedPath, entry.finalPath))
            return Status::ReparseOrRootEscape;
        if (entry.isDirectory != (kind == anr::EntryKind::Directory)) return Status::IoFailure;
        provider->entries.push_back(std::move(opened.entry));
        return Status::Accepted;
    };
    // Pin each ancestor before the image so a junction added below it is caught
    // by the final-path identity check rather than followed.
    for (auto pos = expected.find(L'\\', 7); pos != expected.npos; pos = expected.find(L'\\', pos + 1)) {
        if (const auto status = pin(expected.substr(0, pos), anr::EntryKind::Directory); status != Status::Accepted)
            return {status, L"ancestor"};
    }
    if (const auto status = pin(expected, anr::EntryKind::File); status != Status::Accepted)
        return {status, L"provider"};
    auto& entry = *provider->entries.back();
    if (entry.size < min_provider_bytes || entry.size > max_provider_bytes)
        return {Status::InvalidSize, std::to_wstring(entry.size)};
    provider->canonicalPath = entry.finalPath;
    std::vector<unsigned char> bytes(static_cast<size_t>(entry.size));
    if (!entry.read(bytes)) return {Status::IoFailure, L"read"};
    const std::array<std::string_view, 1> exports{provider_export};
    if (const auto status = anr::inspect_exports(bytes, exports); status != anr::ValidationStatus::Accepted) {
        switch (status) {
        case anr::ValidationStatus::InvalidPe: return {Status::InvalidPe, L"pe"};
        case anr::ValidationStatus::MissingExport: return {Status::MissingExport, L"MfgXeMFGProviderGetApi"};
        case anr::ValidationStatus::InvalidExport: return {Status::InvalidExport, L"MfgXeMFGProviderGetApi"};
        default: return {Status::InvalidPe, L"pe"};
        }
    }
    provider->sha256 = sha256(bytes);
    if (provider->sha256.empty()) return {Status::HashFailure, L"provider"};

    std::vector<std::wstring> candidates;
    const auto runtime = normalize(xessRuntimeDir);
    if (!runtime.empty()) candidates.push_back(runtime);
    auto sibling = normalize((std::filesystem::path(entry.finalPath).parent_path() / companion_directory).wstring());
    if (candidates.empty() || !equal_nocase(candidates.front(), sibling)) candidates.push_back(std::move(sibling));
    auto result = companions.validate(candidates);
    if (result.status != Status::Accepted || !result.lease)
        return {Status::CompanionRejected, result.detail.empty() ? L"companion" : std::wstring(result.detail.begin(), result.detail.end())};
    provider->companionDirectory = result.lease->directory;
    provider->companions = std::move(result.lease);
    return {Status::Accepted, {}, std::move(provider)};
}

Validation validate_provider(std::wstring_view path, bool experimentalOptIn,
    std::wstring_view xessRuntimeDir, CompanionValidator& companions)
{
    return validate_provider(path, experimentalOptIn, xessRuntimeDir, anr::DiskSource{}, companions);
}
} // namespace nr::fg::xemfg
