// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "anr_guides.h"
#include "fg_backend.h"
#include "xemfg_runtime.h"
#include <cstdio>
#include <string>
#include <string_view>

namespace nr::fg::vfg {
class Gpu;

struct XeMfgOpen {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    Extent size;
    uint64_t generation;
    std::wstring path;
    std::wstring xessRuntimeDir;
    bool optIn;
    uint32_t multiplier;
    FILE* log;
    Gpu* gpu{};
    FILE* submissionTrace{};
    uint64_t frameIntervalNs{};
};

// Opt-in 2x/3x/4x transport. The provider writes each generated frame directly
// into Gpu::syntheticTex, so VfgHudLock, VfgShotChange, LiveAnchorClock,
// PushPacingRepair, AdaptiveMultiplierGate and splitCompare stay identical to
// the swapchain-owning backends. No presentation is owned here.
class XeMfgBackend final : public IFgBackend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool faulted_{};
public:
    XeMfgBackend();
    ~XeMfgBackend() override;
    bool open(const XeMfgOpen&);
    const char* name() const noexcept override { return "xemfg"; }
    unsigned version_tag() const noexcept override { return MFG_XEMFG_PROVIDER_API_VERSION; }
    bool faulted() const noexcept override { return faulted_; }
    uint32_t multiplier_limit() const noexcept;
    size_t max_in_flight_pairs() const noexcept override;
    std::optional<FgPairTicket> submit_pair(const FgPairDesc&, unsigned prevIndex, unsigned currIndex) override;
    bool pair_complete(const FgPairTicket&) const;
    bool subframe_ready(const FgPairTicket&, uint32_t sub) override;
    void resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource*, uint32_t) override;
    void retire_pair(const FgPairTicket&, ID3D12Fence* consumeFence = nullptr,
        uint64_t consumeValue = 0) noexcept override;
    void synchronize() override;
    [[nodiscard]] bool close() noexcept override;
    [[nodiscard]] SharedGuideStats guide_stats() const noexcept;
private:
    bool close_impl();
};
} // namespace nr::fg::vfg
