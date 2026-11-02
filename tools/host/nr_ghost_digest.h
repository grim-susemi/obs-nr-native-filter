// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nr::qa::ghost_digest {
inline constexpr std::string_view algorithm="sha256";
using Digest=std::array<uint8_t,32>;
inline constexpr std::array<std::string_view,17> roles{
    "canonical","encoded","keep","foreign","resolved","current_analysis","reference_analysis",
    "motion","vectors","cost","completed","current_raw","selected_raw","selected","program","preview","projector"};
struct Timing {
    int64_t setupNs=0,hashNs=0,finishNs=0;
    uint64_t bytes=0;
};
// One provider/reusable hash state on the existing drain. Finish resets logical
// state for the next role; no image pointer or previous result is retained.
class Sha256 final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&)=delete;
    Sha256& operator=(const Sha256&)=delete;
    int64_t preparation_ns() const;
    Digest hash(std::span<const uint8_t>,Timing* timing=nullptr);
    Digest hash(std::span<const std::span<const uint8_t>>,Timing* timing=nullptr);
    Digest image(std::span<const uint8_t>,size_t logicalBytes,bool halfFloat,Timing* timing=nullptr);
};
struct Payload {
    std::span<const uint8_t> bytes;
    size_t logicalBytes=0;
    bool halfFloat=false,present=false;
};
// CNG SIMD batches independent complete payloads on the calling drain thread.
// This retains flat SHA256, not a tree digest, and adds no application worker.
class Sha256Batch final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    static constexpr size_t capacity=19; // Seventeen stages and two owned codec payloads.
    Sha256Batch();
    ~Sha256Batch();
    Sha256Batch(const Sha256Batch&)=delete;
    Sha256Batch& operator=(const Sha256Batch&)=delete;
    int64_t preparation_ns() const;
    void images(std::span<const Payload>,std::span<std::optional<Digest>>,Timing* timing=nullptr);
};
std::string hex(const Digest&);
bool valid_hex(std::string_view);
}
