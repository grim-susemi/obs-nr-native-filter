// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include "fg_vfg_session.h"
#include <charconv>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace nr::fg::vfg::pushflow {
namespace fs = std::filesystem;

inline constexpr unsigned steady_interval = 120;
enum class ShaderPath { Unpack, Reconstruct };

// One instance per manual preview, shared by all of its resize generations.
class PreviewMultiplier {
    bool quarantined_{};
public:
    uint32_t effective(uint32_t requested) const noexcept { return quarantined_ ? 1u : requested; }
    void quarantine() noexcept { quarantined_ = true; }
};

struct ModelFacts {
    std::optional<bool> alignCorners, maskForFirst;
    unsigned pad{64};
    bool valid{true};
};

inline ModelFacts read_model_facts(const fs::path& model)
{
    std::ifstream in(fs::path(model).concat(L".txt"));
    ModelFacts facts;
    std::optional<unsigned> pad;
    for (std::string line; std::getline(in, line);) {
        const auto equals = line.find('=');
        if (equals == line.npos) continue;
        const auto key = std::string_view(line).substr(0, equals);
        const auto value = std::string_view(line).substr(equals + 1);
        if (key == "align_corners" || key == "mask_for_first") {
            auto& field = key == "align_corners" ? facts.alignCorners : facts.maskForFirst;
            if (field.has_value() || (value != "0" && value != "1")) facts.valid = false;
            else field = value == "1";
        } else if (key == "padding_mode") {
            if (value != "border") facts.valid = false;
        } else if (key == "pad") {
            unsigned n{};
            const auto end = value.data() + value.size();
            const auto result = std::from_chars(value.data(), end, n);
            if (pad || result.ec != std::errc{} || result.ptr != end ||
                n < 8 || n > 512 || (n & (n - 1)) != 0) facts.valid = false;
            else pad = n;
        }
    }
    facts.pad = pad.value_or(64);
    return facts;
}

inline std::optional<ShaderPath> model_shader(size_t outputs, bool hasNamedMotion, const ModelFacts& facts)
{
    if (!facts.valid) return {};
    if (outputs == 1) return ShaderPath::Unpack;
    if (outputs == 4 && hasNamedMotion && facts.alignCorners.has_value() && facts.maskForFirst.has_value())
        return ShaderPath::Reconstruct;
    return {};
}

inline bool matches_outputs(unsigned expected, size_t actual, ShaderPath shader)
{
    return (!expected || expected == actual) &&
        ((actual == 1 && shader == ShaderPath::Unpack) ||
         (actual == 4 && shader == ShaderPath::Reconstruct));
}

inline fs::path marker_directory()
{
    wchar_t local[4096]{};
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    return size && size < ARRAYSIZE(local) ? fs::path(local) / L"obs-nr-native-filter" : fs::path{};
}

class StartingMarker {
    fs::path directory_;
    uint64_t ownerToken_{};
    unsigned completed_{};
    bool ownsRun_{};

    fs::path starting() const
    {
        return directory_ / nr::fg::vfg::starting_marker_filename(ownerToken_);
    }
    bool arm() const
    {
        std::error_code ec;
        fs::create_directories(directory_, ec);
        if (ec) return false;
        std::ofstream file(starting(), std::ios::trunc);
        file << "pushflow_start_or_teardown\n";
        file.close();
        return !!file;
    }

public:
    enum class Open { Armed, PreviousCrash, Failed };
    explicit StartingMarker(fs::path directory, uint64_t ownerToken = 0)
        : directory_(std::move(directory)), ownerToken_(ownerToken) {}

    Open begin()
    {
        if (directory_.empty()) return Open::Failed;
        const auto legacy = directory_ / L"pushflow-starting.txt";
        const auto quarantine = directory_ / L"pushflow-crash-quarantine.txt";
        std::error_code ec;
        const bool ownExists = fs::exists(starting(), ec);
        if (ec) return Open::Failed;
        const bool legacyExists = ownerToken_ != 0 && fs::exists(legacy, ec);
        if (ec) return Open::Failed;
        const bool quarantined = fs::exists(quarantine, ec);
        if (ec) return Open::Failed;
        if (ownExists || legacyExists || quarantined) {
            std::error_code legacyError, quarantineError;
            if (ownExists) fs::remove(starting(), ec);
            if (legacyExists) fs::remove(legacy, legacyError);
            if (quarantined) fs::remove(quarantine, quarantineError);
            return ec || legacyError || quarantineError ? Open::Failed : Open::PreviousCrash;
        }
        ownsRun_ = arm();
        return ownsRun_ ? Open::Armed : Open::Failed;
    }
    bool steady() { return ++completed_ != steady_interval || clear(); }
    bool begin_teardown(bool userClosing) const { return !ownsRun_ || userClosing || arm(); }
    // User-close marker removal is owned solely by the parent Controller observer.
    bool finish_teardown(bool userClosing, bool clean) const { return !ownsRun_ || userClosing || !clean || clear(); }

    bool clear() const
    {
        if (directory_.empty()) return true;
        std::error_code ec;
        fs::remove(starting(), ec);
        return !ec;
    }
};
} // namespace nr::fg::vfg::pushflow
