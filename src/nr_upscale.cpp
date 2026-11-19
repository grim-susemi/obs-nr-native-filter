// SPDX-License-Identifier: GPL-3.0-only
#include "nr_upscale.h"
#include <array>
#include <stdexcept>

namespace nr {
namespace {
void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("CAS D3D12 initialization failed");
}

__declspec(noinline) NVSDK_NGX_Result seh_allocate(
    decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters) fn, NVSDK_NGX_Parameter** out, uint32_t& seh) {
    seh = 0;
    __try { return fn(out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { seh = GetExceptionCode(); return NVSDK_NGX_Result_FAIL_PlatformError; }
}

__declspec(noinline) NVSDK_NGX_Result seh_destroy(
    decltype(&NVSDK_NGX_D3D12_DestroyParameters) fn, NVSDK_NGX_Parameter* p, uint32_t& seh) {
    seh = 0;
    __try { return fn(p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { seh = GetExceptionCode(); return NVSDK_NGX_Result_FAIL_PlatformError; }
}

__declspec(noinline) NVSDK_NGX_Result seh_create_sr(
    decltype(&NVSDK_NGX_D3D12_CreateFeature) fn, ID3D12GraphicsCommandList* cmd,
    NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out, uint32_t& seh) {
    seh = 0;
    __try { return fn(cmd, NVSDK_NGX_Feature_SuperSampling, params, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { seh = GetExceptionCode(); return NVSDK_NGX_Result_FAIL_PlatformError; }
}

__declspec(noinline) NVSDK_NGX_Result seh_eval_sr(
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) fn, ID3D12GraphicsCommandList* cmd,
    NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* params, uint32_t& seh) {
    seh = 0;
    __try { return fn(cmd, handle, params, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { seh = GetExceptionCode(); return NVSDK_NGX_Result_FAIL_PlatformError; }
}

__declspec(noinline) NVSDK_NGX_Result seh_release_sr(
    decltype(&NVSDK_NGX_D3D12_ReleaseFeature) fn, NVSDK_NGX_Handle* handle, uint32_t& seh) {
    seh = 0;
    __try { return fn(handle); }
    __except (EXCEPTION_EXECUTE_HANDLER) { seh = GetExceptionCode(); return NVSDK_NGX_Result_FAIL_PlatformError; }
}
} // namespace

D3D12CasPass::D3D12CasPass(ID3D12Device* device, std::span<const std::byte> shader) : device_(device) {
    if (!device || shader.empty()) throw std::invalid_argument("CAS shader required");
    std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    std::array<D3D12_ROOT_PARAMETER, 2> params{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, sizeof(CasConstants) / 4};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
    D3D12_ROOT_SIGNATURE_DESC desc{static_cast<UINT>(params.size()), params.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = root_.Get();
    ps.CS = {shader.data(), shader.size()};
    check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline_)));
}

bool D3D12CasPass::record(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* heap,
    const CasConstants& cfg, ID3D12Resource* src, ID3D12Resource* orig, ID3D12Resource* out) {
    if (!cmd || !heap || !src || !out || !cfg.srcW || !cfg.srcH || !cfg.outW || !cfg.outH || src == out)
        return false;
    const auto hd = heap->GetDesc();
    if (hd.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || hd.NumDescriptors < 3 ||
        !(hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return false;
    const auto od = out->GetDesc();
    if (od.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || od.Width != cfg.outW || od.Height != cfg.outH ||
        !(od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
    ID3D12Resource* second = orig ? orig : src;
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (ID3D12Resource* s : {src, second}) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = s->GetDesc().Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(s, &view, cpu);
        cpu.ptr += stride;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uview{};
    uview.Format = od.Format;
    uview.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(out, nullptr, &uview, cpu);

    cmd->SetDescriptorHeaps(1, &heap);
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetPipelineState(pipeline_.Get());
    cmd->SetComputeRoot32BitConstants(0, sizeof(cfg) / 4, &cfg, 0);
    cmd->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    cmd->Dispatch((cfg.outW + 7) / 8, (cfg.outH + 7) / 8, 1);
    return true;
}

D3D12DlssSr::~D3D12DlssSr() {
    release();
}

bool D3D12DlssSr::create(const runtime::Dispatch& dispatch, ID3D12GraphicsCommandList* cmd,
    Extent inSize, Extent outSize, uint32_t model, bool nativeResolution) {
    if (!dispatch.valid() || !cmd || !inSize.width || !inSize.height || !outSize.width || !outSize.height || model >= 3) {
        failed_ = true;
        return false;
    }
    if (nativeResolution && inSize != outSize) {
        failed_ = true;
        return false;
    }
    if (handle_) release();
    dispatch_ = dispatch;
    inW_ = inSize.width; inH_ = inSize.height;
    outW_ = outSize.width; outH_ = outSize.height;
    model_ = model;
    nativeRes_ = nativeResolution;
    failed_ = false;
    lastError_ = 0;

    uint32_t seh = 0;
    NVSDK_NGX_Result res = seh_allocate(dispatch_.allocate, &params_, seh);
    if (res != NVSDK_NGX_Result_Success || !params_ || seh) {
        lastError_ = seh ? seh : static_cast<uint32_t>(res);
        failed_ = true;
        return false;
    }

    int perfQuality = NVSDK_NGX_PerfQuality_Value_Balanced;
    if (nativeRes_) perfQuality = NVSDK_NGX_PerfQuality_Value_DLAA;
    else if (outSize.width >= inSize.width * 2) perfQuality = NVSDK_NGX_PerfQuality_Value_MaxPerf;
    else if (outSize.width * 3 <= inSize.width * 4 + 32) perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality;

    static constexpr NVSDK_NGX_DLSS_Hint_Render_Preset kModelPresets[] = {
        NVSDK_NGX_DLSS_Hint_Render_Preset_K,
        NVSDK_NGX_DLSS_Hint_Render_Preset_M,
        NVSDK_NGX_DLSS_Hint_Render_Preset_L
    };
    const unsigned preset = static_cast<unsigned>(kModelPresets[model_]);
    for (const char* key : {
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
        params_->Set(key, preset);
    }

    const int createFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    params_->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    params_->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    params_->Set(NVSDK_NGX_Parameter_Width, inW_);
    params_->Set(NVSDK_NGX_Parameter_Height, inH_);
    params_->Set(NVSDK_NGX_Parameter_OutWidth, outW_);
    params_->Set(NVSDK_NGX_Parameter_OutHeight, outH_);
    params_->Set(NVSDK_NGX_Parameter_PerfQualityValue, perfQuality);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, createFlags);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);

    res = seh_create_sr(dispatch_.create, cmd, params_, &handle_, seh);
    if (res != NVSDK_NGX_Result_Success || !handle_ || seh) {
        int featInit = 0;
        (void)params_->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &featInit);
        lastError_ = seh ? seh : (featInit ? static_cast<uint32_t>(featInit) : static_cast<uint32_t>(res));
        failed_ = true;
        handle_ = nullptr;
        uint32_t dseh = 0;
        (void)seh_destroy(dispatch_.destroy, params_, dseh);
        params_ = nullptr;
        return false;
    }
    return true;
}

bool D3D12DlssSr::evaluate(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* output,
    ID3D12Resource* depth, ID3D12Resource* motionVectors, bool reset) {
    if (!cmd || !handle_ || !params_ || !color || !output || !depth || !motionVectors) return false;
    params_->Set(NVSDK_NGX_Parameter_Color, color);
    params_->Set(NVSDK_NGX_Parameter_Output, output);
    params_->Set(NVSDK_NGX_Parameter_Depth, depth);
    params_->Set(NVSDK_NGX_Parameter_MotionVectors, motionVectors);
    params_->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    params_->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    params_->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    params_->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
    params_->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    params_->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, inW_);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inH_);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    params_->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);

    uint32_t seh = 0;
    const NVSDK_NGX_Result res = seh_eval_sr(dispatch_.evaluate, cmd, handle_, params_, seh);
    if (res != NVSDK_NGX_Result_Success || seh) {
        lastError_ = seh ? seh : static_cast<uint32_t>(res);
        return false;
    }
    return true;
}

bool D3D12DlssSr::release() {
    bool ok = true;
    if (handle_ && dispatch_.release) {
        uint32_t seh = 0;
        const NVSDK_NGX_Result res = seh_release_sr(dispatch_.release, handle_, seh);
        if (res != NVSDK_NGX_Result_Success || seh) ok = false;
        handle_ = nullptr;
    }
    if (params_ && dispatch_.destroy) {
        uint32_t seh = 0;
        const NVSDK_NGX_Result res = seh_destroy(dispatch_.destroy, params_, seh);
        if (res != NVSDK_NGX_Result_Success || seh) ok = false;
        params_ = nullptr;
    }
    return ok;
}

} // namespace nr
