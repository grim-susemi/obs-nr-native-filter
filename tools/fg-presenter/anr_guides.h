// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_guides.h"
#include <wrl/client.h>

namespace nr::fg::vfg {
class IAnrGuideProvider {
public:
    virtual ~IAnrGuideProvider() = default;
    virtual FlowProblem submit(const FlowInput&) = 0;
    virtual const FlowResult* result() const = 0;
    virtual bool collect(const JobKey&, NV_OF_FENCE_POINT) = 0;
    virtual bool close() = 0;
};

std::unique_ptr<IAnrGuideProvider> make_anr_guides(ID3D12Device*, Extent);

class AnrBgra {
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    Extent size_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvs_, rtvs_;
    std::array<Resource, 2> textures_;
    UINT srvStep_{}, rtvStep_{};
public:
    AnrBgra(ID3D12Device*, Extent);
    ID3D12Resource* get(unsigned slot) const noexcept { return textures_[slot].Get(); }
    void record(ID3D12GraphicsCommandList*, ID3D12Resource*, unsigned slot);
};
} // namespace nr::fg::vfg
