// SPDX-License-Identifier: GPL-3.0-only
#include "nr_stabilize.h"
#include <array>
#include <stdexcept>

namespace nr {
namespace {
void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("stabilize D3D12 initialization failed");
}
}

D3D12StabilizePass::D3D12StabilizePass(ID3D12Device* device, std::span<const std::byte> shader) : device_(device) {
    if (!device || shader.empty()) throw std::invalid_argument("stabilize shader required");
    std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    std::array<D3D12_ROOT_PARAMETER, 2> params{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, sizeof(StabilizeConstants) / 4};
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

bool D3D12StabilizePass::record(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* heap, Extent size,
    const StabilizeSettings& settings, bool hasHistory,
    ID3D12Resource* curOrig, ID3D12Resource* curNr, ID3D12Resource* motion,
    ID3D12Resource* prevOrig, ID3D12Resource* prevTemporal, ID3D12Resource* prevHold,
    ID3D12Resource* outHold, ID3D12Resource* outTemporal, ID3D12Resource* outOrig) {
    if (!cmd || !heap || !size.width || !size.height || !settings.valid() ||
        !curOrig || !curNr || !motion || !prevOrig || !prevTemporal || !prevHold ||
        !outHold || !outTemporal || !outOrig) return false;
    const auto hd = heap->GetDesc();
    if (hd.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || hd.NumDescriptors < 9 ||
        !(hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return false;

    const std::array<ID3D12Resource*, 6> srvs{curOrig, curNr, motion, prevOrig, prevTemporal, prevHold};
    const std::array<ID3D12Resource*, 3> uavs{outHold, outTemporal, outOrig};
    const auto validTex = [&](ID3D12Resource* r, DXGI_FORMAT fmt, bool needUav) {
        const auto d = r->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != size.width || d.Height != size.height ||
            d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1) return false;
        if (fmt != DXGI_FORMAT_UNKNOWN && d.Format != fmt) return false;
        if (needUav && !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        return SUCCEEDED(r->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get() == device_.Get();
    };
    for (size_t i = 0; i < srvs.size(); ++i) {
        const auto fmt = i == 2 ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!validTex(srvs[i], fmt, false)) return false;
    }
    for (auto* u : uavs) {
        if (!validTex(u, DXGI_FORMAT_R16G16B16A16_FLOAT, true)) return false;
    }
    if (outHold == prevHold || outTemporal == prevTemporal || outOrig == prevOrig) return false;

    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (auto* s : srvs) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = s->GetDesc().Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(s, &view, cpu);
        cpu.ptr += stride;
    }
    for (auto* u : uavs) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = u->GetDesc().Format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(u, nullptr, &view, cpu);
        cpu.ptr += stride;
    }

    const StabilizeConstants constants{
        size.width, size.height,
        settings.temporalStrength, settings.holdStrength, settings.holdTolerance,
        hasHistory ? 1u : 0u, 0u, 0u
    };
    cmd->SetDescriptorHeaps(1, &heap);
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetPipelineState(pipeline_.Get());
    cmd->SetComputeRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
    cmd->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    cmd->Dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
    return true;
}
}
