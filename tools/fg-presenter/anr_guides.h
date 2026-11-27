// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_backend.h"
#include "nr_guides.h"
#include "vfg_gpu.h"
#include <wrl/client.h>

namespace nr::fg::vfg {
struct SharedGuideStats {
    uint64_t ofaExecutions = 0; // Local make_anr_guides submissions in this session.
    uint64_t sharedHits = 0;    // NrOwned+Valid pairs consumed without local OFA.
    uint64_t sharedResets = 0;  // Reset/interpolated-start frames using zero motion only.
    uint64_t sharedUnavailable = 0; // NrOwned pairs with no usable imported motion.
};
class IAnrGuideProvider {
public:
    virtual ~IAnrGuideProvider() = default;
    virtual FlowProblem submit(const FlowInput&) = 0;
    virtual const FlowResult* result() const = 0;
    virtual bool collect(const JobKey&, NV_OF_FENCE_POINT) = 0;
    virtual bool close() = 0;
    // Set before each pair. Local-only providers ignore it; the shared provider
    // decides between imported motion and a permitted local fallback.
    virtual void configure(const ImportedFlow&) {}
    // Record the read of the imported pair on the caller's list and make
    // result() valid. False means this provider runs the local path instead.
    virtual bool shared_path(ID3D12GraphicsCommandList*, const FlowImage&, bool, FlowProblem&)
    {
        return false;
    }
    virtual SharedGuideStats stats() const { return {}; }
};

std::unique_ptr<IAnrGuideProvider> make_anr_guides(ID3D12Device*, Extent, uint64_t frameIntervalNs);
// Wraps the local provider with the 3-state single-owner policy:
//   NrOwned+Valid   -> imported motion (scale/sanitize only), local OFA never runs.
//   NrOwned+other   -> no motion; the caller withholds the pair (original frame).
//   anything else   -> local provider runs normally.
std::unique_ptr<IAnrGuideProvider> make_shared_anr_guides(ID3D12Device*, Extent renderSize,
    uint64_t frameIntervalNs, std::unique_ptr<IAnrGuideProvider> local);

class AnrBgra {
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    Extent size_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvs_, rtvs_;
    DescriptorStager stager_;
    std::array<Resource, 2> textures_;
    UINT srvStep_{}, rtvStep_{};
public:
    AnrBgra(ID3D12Device*, Extent);
    ID3D12Resource* get(unsigned slot) const noexcept { return textures_[slot].Get(); }
    void record(ID3D12GraphicsCommandList*, ID3D12Resource*, unsigned slot);
};
} // namespace nr::fg::vfg
