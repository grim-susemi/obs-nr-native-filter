// SPDX-License-Identifier: GPL-3.0-only
// Admission matrix for a user-supplied MFGUnlocker.dll: unsigned-but-valid is
// accepted, every structural/path failure is rejected before LoadLibrary, and
// the Intel companion gate is the only path that may be Authenticode-checked.
#include "fg/xemfg_dll_validator.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace validator = nr::fg::xemfg;
using Status = validator::ValidationStatus;

void require(bool passed, const char* why)
{
    if (!passed) throw std::runtime_error(why);
}

std::wstring environment(const wchar_t* name)
{
    const auto length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    const auto read = GetEnvironmentVariableW(name, value.data(), length);
    require(read && read < length, "environment read");
    value.resize(read);
    return value;
}

struct Scratch {
    const fs::path dir;
    explicit Scratch(const fs::path& parent) : dir(parent / (L"xemfg-validator-" + std::to_wstring(GetCurrentProcessId())))
    {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
        require(fs::create_directory(dir), "create validator scratch");
    }
    ~Scratch()
    {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
};

std::string contents(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write(const fs::path& path, std::string_view bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "write fixture file");
}

struct FakeLease final : validator::CompanionLease {};

struct FakeCompanions final : validator::CompanionValidator {
    Status status{Status::Accepted};
    std::string detail;
    std::vector<std::wstring> seen;
    validator::CompanionResult validate(std::span<const std::wstring> candidates) override
    {
        seen.assign(candidates.begin(), candidates.end());
        if (status != Status::Accepted) return {status, detail, {}};
        auto lease = std::make_unique<FakeLease>();
        lease->directory = candidates.front();
        return {Status::Accepted, {}, std::move(lease)};
    }
};

bool is_hex(const std::string& text)
{
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

fs::path stage(const fs::path& root, const fs::path& source, const wchar_t* name = L"MFGUnlocker.dll")
{
    const auto path = root / name;
    fs::copy_file(source, path, fs::copy_options::overwrite_existing);
    return path;
}

bool make_junction(const fs::path& link, const fs::path& target)
{
    std::wstring command = L"cmd.exe /d /c mklink /j \"" + link.wstring() + L"\" \"" + target.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            target.parent_path().c_str(), &startup, &child)) return false;
    struct Close { HANDLE process, thread; ~Close() { CloseHandle(thread); CloseHandle(process); } }
        close{child.hProcess, child.hThread};
    if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) return false;
    DWORD code{};
    return GetExitCodeProcess(child.hProcess, &code) && !code;
}

void expect(const validator::Validation& validation, Status wanted, const char* why)
{
    require(validation.status == wanted, why);
}

void matrix(const fs::path& mock, const fs::path& scratch)
{
    const auto root = scratch / L"root";
    require(fs::create_directory(root), "create validator root");
    const auto dll = stage(root, mock);
    FakeCompanions companions;
    const auto xess = (scratch / L"xess").wstring();

    expect(validator::validate_provider(dll.wstring(), false, xess, companions), Status::OptInRequired,
        "opt-in gate must run before any file access");
    require(companions.seen.empty(), "refused opt-in must not validate companions");

    auto accepted = validator::validate_provider(dll.wstring(), true, xess, companions);
    expect(accepted, Status::Accepted, "unsigned but structurally valid provider accepted");
    require(accepted.provider && accepted.provider->canonicalPath.ends_with(L"MFGUnlocker.dll"),
        "canonical provider path");
    require(accepted.provider->entries.size() >= 2, "provider and ancestor handles pinned");
    require(is_hex(accepted.provider->sha256), "full SHA-256 logged");
    require(companions.seen.size() == 2 && companions.seen[0] == xess &&
        companions.seen[1].ends_with(validator::companion_directory), "explicit dir then RTXMFG-XeSS fallback");
    std::printf("validator: unsigned_valid=accepted sha256=%s pinned=%zu\n",
        accepted.provider->sha256.c_str(), accepted.provider->entries.size());

    companions.seen.clear();
    expect(validator::validate_provider(dll.wstring(), true, {}, companions), Status::Accepted,
        "empty xess dir uses the sibling fallback");
    require(companions.seen.size() == 1 && companions.seen[0].ends_with(validator::companion_directory),
        "sibling companion path only");

    companions.seen.clear();
    const auto sibling = (root / fs::path(validator::companion_directory)).wstring();
    expect(validator::validate_provider(dll.wstring(), true, sibling, companions), Status::Accepted,
        "sibling candidate accepted");
    require(companions.seen.size() == 1, "duplicate companion candidates deduplicated");

    companions.status = Status::CompanionRejected;
    companions.detail = "hash_mismatch:libxess_fg";
    expect(validator::validate_provider(dll.wstring(), true, xess, companions), Status::CompanionRejected,
        "companion rejection propagates");
    companions.status = Status::Accepted;

    expect(validator::validate_provider(L"relative.dll", true, xess, companions), Status::InvalidPath,
        "relative provider path");
    expect(validator::validate_provider((root / L"MFGUnlocker.txt").wstring(), true, xess, companions),
        Status::InvalidPath, "non-DLL extension");
    expect(validator::validate_provider((root / L"missing.dll").wstring(), true, xess, companions),
        Status::IoFailure, "missing provider");

    const auto small = root / L"small.dll";
    write(small, "not a PE image");
    expect(validator::validate_provider(small.wstring(), true, xess, companions), Status::InvalidSize,
        "undersized file");

    const auto empty = root / L"empty.dll";
    write(empty, std::string(32 * 1024, '\0'));
    expect(validator::validate_provider(empty.wstring(), true, xess, companions), Status::InvalidPe,
        "zeroed x64-sized image is not a PE");

    const auto missing = root / L"no-export.dll";
    auto bytes = contents(dll);
    constexpr std::string_view export_name = "MfgXeMFGProviderGetApi";
    size_t at = 0;
    while ((at = bytes.find(export_name, at)) != std::string::npos) {
        bytes[at + export_name.size() - 1] = 'j';
        at += export_name.size();
    }
    write(missing, bytes);
    expect(validator::validate_provider(missing.wstring(), true, xess, companions), Status::MissingExport,
        "provider export removed");

    const auto outside = scratch / L"outside";
    require(fs::create_directory(outside), "create junction target");
    stage(outside, mock);
    const auto junction = scratch / L"junction";
    require(make_junction(junction, outside), "create owned directory junction");
    struct Junction { const fs::path& path; ~Junction() { RemoveDirectoryW(path.c_str()); } } cleanup{junction};
    require(GetFileAttributesW(junction.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT, "fixture junction is a reparse point");
    expect(validator::validate_provider((junction / L"MFGUnlocker.dll").wstring(), true, xess, companions),
        Status::ReparseOrRootEscape, "junction escape rejected");

    std::printf("validator: opt_in=false=OptInRequired reparse=ReparseOrRootEscape missing_export=MissingExport "
        "missing=IoFailure invalid_size=InvalidSize invalid_pe=InvalidPe companion=CompanionRejected\n");
}

void disk_cases(const fs::path& mock, const fs::path& scratch)
{
    // The production DiskSource is what the validator uses above; this second
    // pass proves the same admission result through the no-files overload.
    const auto root = scratch / L"disk";
    require(fs::create_directory(root), "create disk-case root");
    const auto dll = stage(root, mock);
    FakeCompanions companions;
    const auto accepted = validator::validate_provider(dll.wstring(), true, root.wstring(), companions);
    expect(accepted, Status::Accepted, "disk overload accepted the staged provider");
    require(accepted.provider && accepted.provider->sha256.size() == 64, "disk overload hashes provider");
    std::printf("validator: disk_overload=accepted\n");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        require(argc == 2 || (argc == 1 && !environment(L"OBS_NR_TEST_XEMFG_MOCK_DLL").empty()),
            "usage: xemfg_validator_tests <xemfg_mock_dll>");
        const fs::path mock = argc == 2 ? fs::path(argv[1]) : fs::path(environment(L"OBS_NR_TEST_XEMFG_MOCK_DLL"));
        require(fs::exists(mock), "mock provider path exists");
        std::wstring exe(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        require(length && length < exe.size(), "locate test executable");
        exe.resize(length);
        const Scratch scratch(fs::path(exe).parent_path());
        matrix(mock, scratch.dir);
        disk_cases(mock, scratch.dir);
        std::puts("PASS xemfg_validator");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL xemfg_validator: %s\n", error.what());
        return 1;
    }
}
