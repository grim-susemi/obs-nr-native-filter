// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_vfg_session.h"
#include "vfg_content.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>

namespace nr::fg::vfg {
class VfgHudLock final {
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> update_, compose_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views_;
    Resource state_, scratch_, share_;
    std::array<Resource, content_input_count> inputs_;
    std::array<Resource, 2 * (max_multiplier - 1)> generated_;
    UINT width_{}, height_{}, stride_{};
    D3D12_GPU_DESCRIPTOR_HANDLE table(unsigned index) const;
public:
    VfgHudLock(ID3D12Device*, const std::array<Resource, content_input_count>& inputs,
        const std::array<Resource, 2 * (max_multiplier - 1)>& generated);
    void update(ID3D12GraphicsCommandList*, unsigned current, unsigned previous, bool reset);
    // generated indexes the 2-deep bank: slotParity * (max_multiplier - 1) + subframe.
    void compose(ID3D12GraphicsCommandList*, unsigned generated, unsigned held);
    ID3D12Resource* state() const noexcept { return state_.Get(); }
    ID3D12Resource* output() const noexcept { return scratch_.Get(); }
};
}
