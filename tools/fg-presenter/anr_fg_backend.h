// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "anr_guides.h"
#include "fg_backend.h"
#include <cstdio>
#include <string_view>

namespace nr::fg::vfg {
class Gpu;
struct AnrOpen {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    Extent size;
    uint64_t generation;
    std::wstring_view path;
    bool optIn;
    FILE* log;
    Gpu* gpu{};
    FILE* submissionTrace{};
};

class AnrFgBackend final : public IFgBackend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool faulted_{};
public:
    enum class Output { Pending, Ready, Suppressed };
    AnrFgBackend();
    ~AnrFgBackend() override;
    bool open(const AnrOpen&, std::unique_ptr<IAnrGuideProvider> guides = {});
    const char* name() const noexcept override { return "anr-experimental"; }
    unsigned version_tag() const noexcept override { return 15; }
    bool faulted() const noexcept override { return faulted_; }
    uint32_t multiplier_limit() const noexcept;
    std::optional<FgPairTicket> submit_pair(const FgPairDesc&, unsigned prevIndex, unsigned currIndex) override;
    bool pair_complete(const FgPairTicket&) const;
    Output output(const FgPairTicket&, uint32_t sub);
    bool subframe_ready(const FgPairTicket& ticket, uint32_t sub) override { return output(ticket, sub) == Output::Ready; }
    void resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource*, uint32_t) override;
    // The experimental ANR path owns one guides/commands/dispatch bank, so its
    // single pair must complete before a second pair can be admitted.
    size_t max_in_flight_pairs() const noexcept override { return 1; }
    void retire_pair(const FgPairTicket&, ID3D12Fence* consumeFence = nullptr, uint64_t consumeValue = 0) noexcept override;
    void synchronize() override;
    [[nodiscard]] bool close();
};
} // namespace nr::fg::vfg
