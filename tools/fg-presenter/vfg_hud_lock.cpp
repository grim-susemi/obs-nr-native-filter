// SPDX-License-Identifier: GPL-3.0-only
#include "vfg_hud_lock.h"
#include "vfg_gpu.h"
#include "vfg_hud_lock_update_shader.h"
#include "vfg_hud_lock_compose_shader.h"
#include <algorithm>

namespace nr::fg::vfg {
namespace {
constexpr unsigned descriptors_per_table = 5;
constexpr unsigned generated_count = 2 * (max_multiplier - 1);
constexpr unsigned update_tables = content_input_count * content_input_count;
constexpr unsigned compose_tables = generated_count * content_input_count;
constexpr unsigned update_descriptors = update_tables * descriptors_per_table;
static_assert(content_input_count == 4 && max_multiplier - 1 == 3);
// update 4x4 = 80 plus compose 6x4 = 120 descriptors, one heap for both passes.
static_assert((update_tables + compose_tables) * descriptors_per_table == 200);

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
    D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return barrier;
}
}

VfgHudLock::VfgHudLock(ID3D12Device* device, const std::array<Resource, content_input_count>& inputs,
    const std::array<Resource, generated_count>& generated) : inputs_(inputs), generated_(generated)
{
    const auto inputDesc = inputs_[0]->GetDesc();
    width_ = static_cast<UINT>(inputDesc.Width);
    height_ = inputDesc.Height;
    D3D12_DESCRIPTOR_RANGE ranges[5]{};
    D3D12_ROOT_PARAMETER params[6]{};
    for (UINT i = 0; i < 5; ++i) {
        ranges[i] = {i == 4 ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            1, i == 4 ? 0u : i, 0, 0};
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable = {1, &ranges[i]};
    }
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[5].Constants = {0, 0, 8};
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 6;
    signature.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr),
        "HUD lock root serialization");
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&root_)), "HUD lock root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_.Get();
    pipeline.CS = {vfg_hud_lock_update_shader, sizeof(vfg_hud_lock_update_shader)};
    check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&update_)), "HUD update pipeline");
    pipeline.CS = {vfg_hud_lock_compose_shader, sizeof(vfg_hud_lock_compose_shader)};
    check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&compose_)), "HUD compose pipeline");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    auto desc = inputDesc;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&state_)), "HUD state texture");
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratch_)), "HUD scratch texture");
    desc.Width = desc.Height = 1;
    desc.Format = DXGI_FORMAT_R16_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&share_)), "HUD dummy texture");
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        (update_tables + compose_tables) * descriptors_per_table, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&views_)), "HUD visible descriptors");
    DescriptorStager staging(device);
    stride_ = device->GetDescriptorHandleIncrementSize(hd.Type);
    auto dest = views_->GetCPUDescriptorHandleForHeapStart();
    // Every table is written once, before submission. It stays immutable for
    // the lifetime of this generation, including while input content rotates.
    const auto write_table = [&](ID3D12Resource* first, ID3D12Resource* second, ID3D12Resource* third,
        ID3D12Resource* fourth, ID3D12Resource* target) {
        ID3D12Resource* resources[]{first, second, third, fourth};
        for (auto* resource : resources) {
            staging.write_srv(resource, nullptr, dest);
            dest.ptr += stride_;
        }
        staging.write_uav(target, dest);
        dest.ptr += stride_;
    };
    for (unsigned current = 0; current < inputs_.size(); ++current)
        for (unsigned previous = 0; previous < inputs_.size(); ++previous)
            write_table(inputs_[current].Get(), inputs_[previous].Get(), share_.Get(), share_.Get(), state_.Get());
    for (unsigned bank = 0; bank < generated_.size(); ++bank) {
        if (!generated_[bank]) {
            // An original-only Gpu has no synthetic images or compose consumers.
            dest.ptr += SIZE_T{content_input_count * descriptors_per_table} * stride_;
            continue;
        }
        for (unsigned held = 0; held < inputs_.size(); ++held)
            write_table(generated_[bank].Get(), inputs_[held].Get(), state_.Get(), share_.Get(), scratch_.Get());
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE VfgHudLock::table(unsigned index) const
{
    auto handle = views_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64{index} * stride_;
    return handle;
}

void VfgHudLock::update(ID3D12GraphicsCommandList* cmd, unsigned current, unsigned previous, bool reset)
{
    D3D12_RESOURCE_BARRIER barriers[]{
        transition(inputs_[current].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(state_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    cmd->ResourceBarrier(2, barriers);
    auto prev = transition(inputs_[previous].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (current != previous) cmd->ResourceBarrier(1, &prev);
    ID3D12DescriptorHeap* heaps[]{views_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetPipelineState(update_.Get());
    const unsigned base = (current * content_input_count + previous) * descriptors_per_table;
    for (unsigned i = 0; i < descriptors_per_table; ++i) cmd->SetComputeRootDescriptorTable(i, table(base + i));
    const UINT constants[]{width_, height_, reset ? 1u : 0u, 0, 1, 0, 0, 0};
    cmd->SetComputeRoot32BitConstants(5, 8, constants, 0);
    cmd->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
    for (auto& b : barriers) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cmd->ResourceBarrier(2, barriers);
    std::swap(prev.Transition.StateBefore, prev.Transition.StateAfter);
    if (current != previous) cmd->ResourceBarrier(1, &prev);
}

void VfgHudLock::compose(ID3D12GraphicsCommandList* cmd, unsigned generated, unsigned held)
{
    D3D12_RESOURCE_BARRIER barriers[]{
        transition(generated_[generated].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(inputs_[held].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(state_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(scratch_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    cmd->ResourceBarrier(4, barriers);
    ID3D12DescriptorHeap* heaps[]{views_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetPipelineState(compose_.Get());
    const unsigned base = update_descriptors +
        (generated * content_input_count + held) * descriptors_per_table;
    for (unsigned i = 0; i < descriptors_per_table; ++i) cmd->SetComputeRootDescriptorTable(i, table(base + i));
    const UINT constants[]{width_, height_, 0, 1, 0, 0, 0, 0};
    cmd->SetComputeRoot32BitConstants(5, 8, constants, 0);
    cmd->Dispatch((width_ + 15) / 16, (height_ + 15) / 16, 1);
    for (auto& b : barriers) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cmd->ResourceBarrier(4, barriers);
}
}
