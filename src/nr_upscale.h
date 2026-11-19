// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include "nr_runtime.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <span>

namespace nr {

struct CasConstants {
    uint32_t mode, srcW, srcH, outW;
    uint32_t outH, colorSpace, alphaMode, srgbTransfer;
    float sharpness, linearScale, pad0{0}, pad1{0};
};
static_assert(sizeof(CasConstants) == 48);

class D3D12CasPass final {
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
public:
    D3D12CasPass(ID3D12Device*, std::span<const std::byte> shader);
    // mode 0: src(srcW x srcH) -> out(outW x outH) FP16 (optional sRGB->Linear)
    // mode 1: src(outW x outH linear) + orig(srcW x srcH) -> out(outW x outH final)
    // mode 2: src(outW x outH) -> out(outW x outH) FP16 CAS-only
    bool record(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* heap,
        const CasConstants& cfg, ID3D12Resource* src, ID3D12Resource* orig, ID3D12Resource* out);
};

class D3D12DlssSr final {
    runtime::Dispatch dispatch_{};
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    uint32_t inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;
    uint32_t model_ = 1;
    bool nativeRes_ = false;
    bool failed_ = false;
    uint32_t lastError_ = 0;
public:
    D3D12DlssSr() = default;
    ~D3D12DlssSr();
    D3D12DlssSr(const D3D12DlssSr&) = delete;
    D3D12DlssSr& operator=(const D3D12DlssSr&) = delete;

    bool create(const runtime::Dispatch& dispatch, ID3D12GraphicsCommandList* cmd,
        Extent inSize, Extent outSize, uint32_t model, bool nativeResolution);
    bool evaluate(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* output,
        ID3D12Resource* depth, ID3D12Resource* motionVectors, bool reset);
    bool release();

    bool created() const { return handle_ != nullptr; }
    bool failed() const { return failed_; }
    uint32_t last_error() const { return lastError_; }
    Extent input_size() const { return {inW_, inH_}; }
    Extent output_size() const { return {outW_, outH_}; }
    uint32_t model() const { return model_; }
    bool native_resolution() const { return nativeRes_; }
};

} // namespace nr
