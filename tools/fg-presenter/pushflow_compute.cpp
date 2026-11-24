// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): Based on PushFPS (GPL-3.0-or-later), modified 2026-11-22 for the OBS presenter.
#include "pushflow_compute.h"
#include <bit>

namespace nr::fg::vfg::pushflow {
namespace {
constexpr auto shaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto unordered = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
constexpr UINT textures = static_cast<UINT>(content_input_count);
constexpr UINT clearIndex = textures + max_multiplier - 1;
constexpr UINT nullInput = clearIndex + 1;

enum Root : UINT { Constants, Previous, Current, Buffer0, Buffer1, Buffer2, MotionOutput, Count };
}

void Command::prepare(ID3D12Device* device)
{
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "PushFlow allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
        "PushFlow list");
    check(list->Close(), "PushFlow initial list close");
}

ID3D12GraphicsCommandList* Command::begin() const
{
    check(allocator->Reset(), "PushFlow allocator reset");
    check(list->Reset(allocator.Get(), nullptr), "PushFlow list reset");
    return list.Get();
}

void Command::submit(ID3D12CommandQueue* queue) const
{
    check(list->Close(), "PushFlow list close");
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
}

Compute::Compute(Gpu& gpu, const OrtState& ort, const Config& config)
    : gpu_(gpu), ort_(ort), geometry_(config.geometry), facts_(config.facts)
{
    auto* device = gpu_.d3d_device();
    D3D12_DESCRIPTOR_RANGE ranges[]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0},
    };
    // A descriptor table at u1 aliases the motion PSO's root UAV at u1,
    // so each shader path has its own root signature layout.
    const bool motion = ort_.shader == ShaderPath::Reconstruct;
    ranges[2].BaseShaderRegister = motion ? 3 : 1;
    std::array<D3D12_ROOT_PARAMETER, Count> parameters{};
    parameters[Constants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[Constants].Constants = {0, 0, 12};
    for (auto root : {Previous, Current}) {
        parameters[root].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[root].DescriptorTable = {1, &ranges[root - Previous]};
    }
    for (UINT i = 0; i < (motion ? 3u : 1u); ++i) {
        parameters[Buffer0 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[Buffer0 + i].Descriptor = {i, 0};
    }
    const UINT outputRoot = motion ? MotionOutput : Buffer1;
    parameters[outputRoot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[outputRoot].DescriptorTable = {1, &ranges[2]};
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = outputRoot + 1;
    rootDesc.pParameters = parameters.data();
    ComPtr<ID3DBlob> blob;
    check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr), "PushFlow serialize root");
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)),
        "PushFlow root signature");
    const auto& shader = gpu_.pushflow_shader_bytecode();
    const auto makePso = [&](D3D12_SHADER_BYTECODE bytecode, ComPtr<ID3D12PipelineState>& pso) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_.Get();
        desc.CS = bytecode;
        check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "PushFlow compute pipeline");
    };
    makePso(shader.pack, pack_);
    makePso(motion ? shader.reconstruct : shader.unpack, resolve_);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = nullInput + 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&views_)), "PushFlow shader views");
    heap.NumDescriptors = 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&clearCpu_)), "PushFlow clear view");
    stride_ = device->GetDescriptorHandleIncrementSize(heap.Type);
    DescriptorStager stager(device);
    auto cpu = views_->GetCPUDescriptorHandleForHeapStart();
    for (const auto& input : gpu_.input) {
        stager.write_srv(input.Get(), nullptr, cpu);
        cpu.ptr += stride_;
    }
    for (const auto& target : gpu_.syntheticTex) {
        stager.write_uav(target.Get(), cpu);
        cpu.ptr += stride_;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC clear{};
    clear.Format = DXGI_FORMAT_R32_TYPELESS;
    clear.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    clear.Buffer.NumElements = ort_.tensors[0].elements;
    clear.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    auto* input = ort_.tensors[0].buffer.Get();
    device->CreateUnorderedAccessView(input, nullptr, &clear, clearCpu_->GetCPUDescriptorHandleForHeapStart());
    device->CopyDescriptorsSimple(1, cpu, clearCpu_->GetCPUDescriptorHandleForHeapStart(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu.ptr += stride_;
    // The pack PSO still declares both SRVs when only the timestep is written.
    // Bind a typed null view so this dispatch has no reference to an IPC slot.
    D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};
    nullView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullView.Texture2D.MipLevels = 1;
    stager.write_srv(nullptr, &nullView, cpu);
}

D3D12_GPU_DESCRIPTOR_HANDLE Compute::view(UINT offset) const
{
    auto handle = views_->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64{offset} * stride_;
    return handle;
}

void Compute::clear_input(ID3D12GraphicsCommandList* cmd) const
{
    ID3D12DescriptorHeap* heaps[]{views_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    auto* input = ort_.tensors[0].buffer.Get();
    constexpr UINT zeros[4]{};
    cmd->ClearUnorderedAccessViewUint(view(clearIndex), clearCpu_->GetCPUDescriptorHandleForHeapStart(),
        input, zeros, 0, nullptr);
    const auto barrier = uav(input);
    cmd->ResourceBarrier(1, &barrier);
}

void Compute::setup(ID3D12GraphicsCommandList* cmd, ID3D12PipelineState* pso, const FgPairTicket* source) const
{
    ID3D12DescriptorHeap* heaps[]{views_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetPipelineState(pso);
    cmd->SetComputeRootDescriptorTable(Previous, view(source ? source->prevIndex : nullInput));
    cmd->SetComputeRootDescriptorTable(Current, view(source ? source->currIndex : nullInput));
}

void Compute::transition_inputs(ID3D12GraphicsCommandList* cmd, const FgPairTicket& source, bool entering) const
{
    const auto from = entering ? common : shaderRead, to = entering ? shaderRead : common;
    const D3D12_RESOURCE_BARRIER barriers[]{transition(gpu_.input[source.prevIndex].Get(), from, to),
        transition(gpu_.input[source.currIndex].Get(), from, to)};
    cmd->ResourceBarrier(ARRAYSIZE(barriers), barriers);
}

void Compute::pack(ID3D12GraphicsCommandList* cmd, const Dispatch& work) const
{
    const auto* source = input_reads(ort_.shader, work.sub).pack ? &work.ticket : nullptr;
    auto* input = ort_.tensors[0].buffer.Get();
    if (source) transition_inputs(cmd, *source, true);
    const auto tensorBarrier = uav(input);
    cmd->ResourceBarrier(1, &tensorBarrier);
    setup(cmd, pack_.Get(), source);
    cmd->SetComputeRootUnorderedAccessView(Buffer0, input->GetGPUVirtualAddress());
    const auto& g = geometry_;
    const float time = static_cast<float>(work.sub) / static_cast<float>(work.ticket.activeMultiplier);
    const UINT data[]{g.srcW, g.srcH, g.padW, g.padH, g.modelW, g.modelH, 0,
        static_cast<UINT>(ort_.inputShape[1]), std::bit_cast<UINT>(time), source ? 0u : 1u, 0, 0};
    cmd->SetComputeRoot32BitConstants(Constants, ARRAYSIZE(data), data, 0);
    cmd->Dispatch((g.padW + 15) / 16, (g.padH + 15) / 16, 1);
    if (source) transition_inputs(cmd, *source, false);
    cmd->ResourceBarrier(1, &tensorBarrier);
}

void Compute::resolve(ID3D12GraphicsCommandList* cmd, const Dispatch& work) const
{
    const auto* source = input_reads(ort_.shader, work.sub).resolve ? &work.ticket : nullptr;
    auto* target = gpu_.syntheticTex[work.sub - 1].Get();
    const auto count = source ? 3u : 1u, first = source ? 2u : 1u;
    setup(cmd, resolve_.Get(), source);
    for (UINT i = 0; i < count; ++i) {
        auto* tensor = ort_.tensors[first + i].buffer.Get();
        const auto barrier = uav(tensor);
        cmd->ResourceBarrier(1, &barrier);
        cmd->SetComputeRootUnorderedAccessView(Buffer0 + i, tensor->GetGPUVirtualAddress());
    }
    if (source) transition_inputs(cmd, *source, true);
    const auto toUav = transition(target, common, unordered);
    cmd->ResourceBarrier(1, &toUav);
    const auto& g = geometry_;
    const UINT data[]{g.srcW, g.srcH, g.padW, g.padH, g.modelW, g.modelH, 0,
        facts_.alignCorners.value_or(false), facts_.maskForFirst.value_or(false), 0, 0, 0};
    cmd->SetComputeRoot32BitConstants(Constants, ARRAYSIZE(data), data, 0);
    cmd->SetComputeRootDescriptorTable(source ? MotionOutput : Buffer1, view(textures + work.sub - 1));
    cmd->Dispatch((g.srcW + 15) / 16, (g.srcH + 15) / 16, 1);
    const auto toCommon = transition(target, unordered, common);
    cmd->ResourceBarrier(1, &toCommon);
    if (source) transition_inputs(cmd, *source, false);
    // Finish UAV reads before a later RunWithBinding writes these allocations.
    for (UINT i = 0; i < count; ++i) {
        const auto barrier = uav(ort_.tensors[first + i].buffer.Get());
        cmd->ResourceBarrier(1, &barrier);
    }
}
} // namespace nr::fg::vfg::pushflow
