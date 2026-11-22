// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace nr::fg::vfg {
class VfgShotChange final {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> results_;
    Microsoft::WRL::ComPtr<ID3D12Resource> zero_;
    bool initialized_{};
public:
    struct Pair {
        D3D12_GPU_DESCRIPTOR_HANDLE previous, current;
        uint32_t width, height, parity, compare;
    };
    explicit VfgShotChange(ID3D12Device*);
    void record(ID3D12GraphicsCommandList*, const Pair&, ID3D12Resource* readback);
    void replace_on_cut(ID3D12GraphicsCommandList*, ID3D12Resource* display,
        ID3D12Resource* current, uint32_t parity) const;
};
} // namespace nr::fg::vfg
