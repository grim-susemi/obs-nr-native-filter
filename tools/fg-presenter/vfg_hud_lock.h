// SPDX-License-Identifier: GPL-3.0-only
#pragma once
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
    std::array<Resource, 3> inputs_, generated_;
    UINT width_{}, height_{}, stride_{};
    D3D12_GPU_DESCRIPTOR_HANDLE table(unsigned index) const;
public:
    VfgHudLock(ID3D12Device*, const std::array<Resource, 3>& inputs,
        const std::array<Resource, 3>& generated);
    void update(ID3D12GraphicsCommandList*, unsigned current, unsigned previous, bool reset);
    void compose(ID3D12GraphicsCommandList*, unsigned generated, unsigned held);
    ID3D12Resource* state() const noexcept { return state_.Get(); }
    ID3D12Resource* output() const noexcept { return scratch_.Get(); }
};
}
