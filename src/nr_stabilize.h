// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <span>

namespace nr {
struct StabilizeConstants {
    uint32_t width, height;
    float temporalStrength;
    float holdStrength;
    float holdTolerance;
    uint32_t hasHistory;
    uint32_t pad0{0}, pad1{0};
};
static_assert(sizeof(StabilizeConstants) == 32);

class D3D12StabilizePass final {
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
public:
    D3D12StabilizePass(ID3D12Device*, std::span<const std::byte> shader);
    bool record(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* heap, Extent size,
        const StabilizeSettings& settings, bool hasHistory,
        ID3D12Resource* curOrig, ID3D12Resource* curNr, ID3D12Resource* motion,
        ID3D12Resource* prevOrig, ID3D12Resource* prevTemporal, ID3D12Resource* prevHold,
        ID3D12Resource* outHold, ID3D12Resource* outTemporal, ID3D12Resource* outOrig);
};
}
