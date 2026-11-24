// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "pushflow_ort.h"
#include "vfg_gpu.h"
#include "fg_backend.h"

namespace nr::fg::vfg::pushflow {
struct InputReads {
    bool pack;
    bool resolve;
};

constexpr InputReads input_reads(ShaderPath shader, unsigned sub) noexcept
{
    return {sub == 1, shader == ShaderPath::Reconstruct};
}

inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return barrier;
}

inline D3D12_RESOURCE_BARRIER uav(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

struct Command {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    void prepare(ID3D12Device*);
    ID3D12GraphicsCommandList* begin() const;
    void submit(ID3D12CommandQueue*) const;
};

class Compute {
    Gpu& gpu_;
    const OrtState& ort_;
    const Geometry geometry_;
    const ModelFacts facts_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12DescriptorHeap> views_, clearCpu_;
    ComPtr<ID3D12PipelineState> pack_, resolve_;
    UINT stride_{};

    D3D12_GPU_DESCRIPTOR_HANDLE view(UINT offset) const;
    void setup(ID3D12GraphicsCommandList*, ID3D12PipelineState*, const FgPairTicket* source) const;
    void transition_inputs(ID3D12GraphicsCommandList*, const FgPairTicket&, bool entering) const;
public:
    struct Config { Geometry geometry; ModelFacts facts; };
    struct Dispatch {
        const FgPairTicket& ticket;
        unsigned sub;
    };
    Compute(Gpu&, const OrtState&, const Config&);
    void clear_input(ID3D12GraphicsCommandList*) const;
    void pack(ID3D12GraphicsCommandList*, const Dispatch&) const;
    void resolve(ID3D12GraphicsCommandList*, const Dispatch&) const;
};
} // namespace nr::fg::vfg::pushflow
