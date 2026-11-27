// SPDX-License-Identifier: GPL-3.0-only
// Adapted from PushFPS VfgBackend, modified 2026-11-22.
#include "vfg_shot_change.h"
#include "vfg_gpu.h"
#include "vfg_shot_change_shader.h"
#include <algorithm>
#include <cstring>

namespace nr::fg::vfg {
namespace {
constexpr uint32_t results_bytes = VfgShotChange::slot_count * VfgShotChange::slot_bytes;
constexpr uint64_t readback_base = 256; // Content-compare flags precede the windows.

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
}

VfgShotChange::VfgShotChange(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    D3D12_ROOT_PARAMETER parameters[4]{};
    for (UINT i = 0; i < 2; ++i) {
        ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0, 0};
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = {1, &ranges[i]};
    }
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants = {0, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 4;
    signature.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr),
        "shot change root signature serialize");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&root_)), "shot change root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_.Get();
    pipeline.CS = {vfg_shot_change_shader, sizeof(vfg_shot_change_shader)};
    check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_)), "shot change pipeline");

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = results_bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&results_)), "shot change results");
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&zero_)), "shot change zero upload");
    void* mapped{};
    const D3D12_RANGE noRead{0, 0}, written{0, results_bytes};
    check(zero_->Map(0, &noRead, &mapped), "shot change zero map");
    std::memset(mapped, 0, results_bytes);
    zero_->Unmap(0, &written);
}

void VfgShotChange::record(ID3D12GraphicsCommandList* cmd, const Pair& pair, ID3D12Resource* readback)
{
    if (pair.parity >= slot_count) return;
    if (!initialized_) {
        auto barrier = transition(results_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &barrier);
        cmd->CopyBufferRegion(results_.Get(), 0, zero_.Get(), 0, results_bytes);
        barrier = transition(results_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        cmd->ResourceBarrier(1, &barrier);
        initialized_ = true;
    }
    const uint32_t offset = predicate_offset(pair.parity);
    auto barrier = transition(results_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(1, &barrier);
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetPipelineState(pipeline_.Get());
    cmd->SetComputeRootDescriptorTable(0, pair.previous);
    cmd->SetComputeRootDescriptorTable(1, pair.current);
    cmd->SetComputeRootUnorderedAccessView(2, results_->GetGPUVirtualAddress() + offset);
    // The shader writes its predicate and diagnostic vector at the head of the
    // window it is bound to, so the parity constant is always zero here.
    const uint32_t constants[]{pair.width, pair.height, 0, pair.compare};
    cmd->SetComputeRoot32BitConstants(3, 4, constants, 0);
    cmd->Dispatch(1, 1, 1);
    barrier = transition(results_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &barrier);
    cmd->CopyBufferRegion(readback, readback_base + offset, results_.Get(), offset, slot_bytes);
    barrier = transition(results_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    cmd->ResourceBarrier(1, &barrier);
}

void VfgShotChange::replace_on_cut(ID3D12GraphicsCommandList* cmd, ID3D12Resource* display,
    ID3D12Resource* current, uint32_t parity) const
{
    if (parity >= slot_count) return;
    D3D12_RESOURCE_BARRIER barriers[]{
        transition(results_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PREDICATION),
        transition(display, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        transition(current, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)
    };
    cmd->ResourceBarrier(3, barriers);
    cmd->SetPredication(results_.Get(), predicate_offset(parity), D3D12_PREDICATION_OP_EQUAL_ZERO);
    cmd->CopyResource(display, current);
    cmd->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    for (auto& barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(3, barriers);
}
} // namespace nr::fg::vfg
