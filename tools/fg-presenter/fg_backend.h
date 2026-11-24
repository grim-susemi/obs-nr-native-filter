// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <optional>

namespace nr::fg::vfg {
struct FgPairTicket {
    const uint64_t pairId;
    const unsigned prevIndex, currIndex;
    const uint32_t activeMultiplier, lastSubmittedSub, slotParity;
    bool operator==(const FgPairTicket&) const = default;
};

struct FgPairDesc {
    ID3D12Resource* prevTex;
    ID3D12Resource* currTex;
    uint64_t prevSerial, currSerial;
    uint32_t effectiveMultiplier;
    bool discontinuity;
    bool shotCut{};
    // Optional synchronous notifications at staging admission and backend
    // enqueue completion. Enqueue time excludes staging and its notification.
    std::function<void()> onStageQueued{};
    std::function<void(uint64_t workNs)> onEnqueued{};
};

class IFgBackend {
public:
    virtual ~IFgBackend() = default;
    virtual const char* name() const noexcept = 0;
    virtual unsigned version_tag() const noexcept = 0;
    virtual bool faulted() const noexcept = 0;
    // A transport break is never a valid interpolation pair. A shot cut is
    // still submitted so the shared GPU display path can consume its predicate.
    std::optional<FgPairTicket> try_submit_pair(const FgPairDesc& pair, unsigned prevIndex, unsigned currIndex)
    {
        if (pair.discontinuity) return {};
        return submit_pair(pair, prevIndex, currIndex);
    }
    // Empty means no commands were admitted. The input copies must already
    // be ordered on the draw queue before submitting the pair.
    virtual std::optional<FgPairTicket> submit_pair(const FgPairDesc&, unsigned prevIndex, unsigned currIndex) = 0;
    virtual bool subframe_ready(const FgPairTicket&, uint32_t sub) = 0;
    // Called on an admitted draw list after subframe_ready. outTex is RGBA8
    // Texture2D in COMMON, and must be returned to COMMON before presenting.
    virtual void resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource* outTex,
        uint32_t sub) = 0;
    virtual void synchronize() = 0;
};
} // namespace nr::fg::vfg
