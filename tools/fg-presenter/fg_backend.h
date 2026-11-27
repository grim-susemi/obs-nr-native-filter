// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_ipc.h"
#include "nr_contract.h"
#include <windows.h>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace nr::fg::vfg {
// 60 Hz is the OBS default and the cadence NR's own flow policy assumes when a
// transport does not name its interval.
inline constexpr uint64_t default_frame_interval_ns = 1'000'000'000ull / 60;
inline constexpr uint64_t guide_interval_ns(uint64_t interval) noexcept
{
    return interval ? interval : default_frame_interval_ns;
}
// One GPU timeline point a consumer can wait on. A null fence means the
// producer already ordered the read on the consuming queue.
struct FgFencePoint {
    ID3D12Fence* fence{};
    uint64_t value{};
};
// Motion NR computed for this exact displayed pair, imported over fg_ipc v3.
// guideWidth/guideHeight are the analysis extent; mvScaleX/Y converts one
// analysis pixel of displacement to one render pixel (render / guide).
struct ImportedFlow {
    ID3D12Resource* motion{};
    uint32_t guideWidth{}, guideHeight{};
    float mvScaleX{1.0f}, mvScaleY{1.0f};
    ipc::FlowOwner owner{ipc::FlowOwner::Unavailable};
    ipc::FlowProvenance provenance{ipc::FlowProvenance::Unknown};
    FgFencePoint ready{};
    bool shotCut{}, reset{};
};
// Single-owner routing. NR is active upstream whenever it states NrOwned, so a
// held, dropped or unmatched pair must never fall back to a local analysis that
// would duplicate (and race) NR's device work. Only a consumer-owned fallback,
// or a legacy producer that states nothing at all, may run local OFA.
enum class FlowRouting { Shared, NrUnavailable, LocalFallback };
inline FlowRouting flow_routing(const ImportedFlow& flow) noexcept
{
    switch (flow.owner) {
    case ipc::FlowOwner::NrOwned:
        return flow.provenance == ipc::FlowProvenance::Valid && flow.motion && flow.guideWidth &&
                flow.guideHeight
            ? FlowRouting::Shared
            : FlowRouting::NrUnavailable;
    case ipc::FlowOwner::ConsumerFallback:
        return FlowRouting::LocalFallback;
    case ipc::FlowOwner::Unavailable:
        return flow.provenance == ipc::FlowProvenance::Unknown ? FlowRouting::LocalFallback
                                                             : FlowRouting::NrUnavailable;
    }
    return FlowRouting::LocalFallback;
}
// The presenter schedules at most two pairs through one backend. A backend
// that cannot overlap keeps the default single pair in flight.
inline constexpr size_t batch_capacity = 2;
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
    // Shared NVOF pair routed to this backend. Defaults to a legacy consumer
    // fallback so backends that ignore the field keep their previous behaviour.
    ImportedFlow flow{};
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
    virtual size_t max_in_flight_pairs() const noexcept { return 1; }
    // A pair still occupies its bank while presentation reads it. retire_pair
    // records the consumer completion point: the bank must stay reserved until
    // every queued draw that reads the pair has finished on the GPU as well.
    // Submitting or dropping CPU-side work is not GPU completion.
    virtual void retire_pair(const FgPairTicket& /*ticket*/, ID3D12Fence* /*consumeFence*/ = nullptr,
        uint64_t /*consumeValue*/ = 0) noexcept
    {}
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
    // Teardown is polymorphic: an unconfirmed drain must be reported without
    // unwinding from a destructor. True means every owned resource completed.
    virtual bool close() noexcept { return true; }
};
} // namespace nr::fg::vfg
