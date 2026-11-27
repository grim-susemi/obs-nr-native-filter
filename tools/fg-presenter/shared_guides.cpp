// SPDX-License-Identifier: GPL-3.0-only
// Shared NVOF consumer: the three-state single-owner policy and the
// render-extent scale/sanitize pass. Local OFA runs only for an explicit
// consumer fallback; an NR-owned pair is never re-analysed here.
#include "anr_guides.h"
#include "anr_d3d.h"
#include "shared_flow_scale_shader.h"
#include <bit>
#include <optional>

namespace nr::fg::vfg {
using Microsoft::WRL::ComPtr;
namespace {
// Consumes the Shared NVOF pair over fg_ipc v3. The local provider is retained
// untouched and runs only for an explicit consumer-owned fallback; while NR
// owns the source chain this class never submits local OFA, which is the whole
// point of the single-owner policy.
class SharedGuides final : public IAnrGuideProvider {
    ID3D12Device* device_;
    const Extent render_;
    std::unique_ptr<IAnrGuideProvider> local_;
    ImportedFlow flow_{};
    bool localPair_ = true; // No imported pair configured yet: preserve legacy behaviour.
    std::optional<FlowResult> result_;
    SharedGuideStats stats_{};

    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12DescriptorHeap> heap_, cpuHeap_;
    UINT step_{};
    ComPtr<ID3D12Resource> scaled_, neutralDepth_;
    ComPtr<ID3D12Fence> immediate_;
    bool depthReady_ = false;
    bool pipelineFailed_ = false;

    void ensure_pipeline()
    {
        if (pipeline_ || pipelineFailed_) return;
        try {
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
            ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
            D3D12_ROOT_PARAMETER parameters[3]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[0].Constants.Num32BitValues = 4;
            for (UINT i = 0; i < 2; ++i) {
                parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[i + 1].DescriptorTable.NumDescriptorRanges = 1;
                parameters[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
            }
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.ShaderRegister = 0;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            const D3D12_ROOT_SIGNATURE_DESC signature{3, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
            ComPtr<ID3DBlob> bytes, error;
            check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &error),
                "shared flow scale root signature");
            check(device_->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                IID_PPV_ARGS(&root_)), "shared flow scale root object");
            D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
            state.pRootSignature = root_.Get();
            state.CS = {shared_flow_scale_shader, static_cast<SIZE_T>(sizeof shared_flow_scale_shader)};
            check(device_->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipeline_)),
                "shared flow scale pipeline");
            const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)), "shared flow scale descriptors");
            // ClearUnorderedAccessView* takes its CPU handle from a CPU-only heap
            // and its GPU handle from the shader-visible one; a single
            // shader-visible heap is rejected by the debug layer.
            auto cpuHeap = heap;
            cpuHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            check(device_->CreateDescriptorHeap(&cpuHeap, IID_PPV_ARGS(&cpuHeap_)), "shared flow CPU descriptors");
            step_ = device_->GetDescriptorHandleIncrementSize(heap.Type);
            D3D12_HEAP_PROPERTIES heapProperties{};
            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC motion{};
            motion.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            motion.Width = render_.width;
            motion.Height = render_.height;
            motion.DepthOrArraySize = motion.MipLevels = 1;
            motion.Format = DXGI_FORMAT_R16G16_FLOAT;
            motion.SampleDesc.Count = 1;
            motion.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            check(device_->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &motion,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scaled_)), "shared flow scaled motion");
            auto depth = motion;
            depth.Format = DXGI_FORMAT_R32_FLOAT;
            check(device_->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &depth,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&neutralDepth_)), "shared flow neutral depth");
            check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&immediate_)),
                "shared flow immediate fence");
            auto uav = heap_->GetCPUDescriptorHandleForHeapStart();
            uav.ptr += step_ * 2;
            device_->CreateUnorderedAccessView(neutralDepth_.Get(), nullptr, nullptr, uav);
        } catch (...) {
            pipelineFailed_ = true;
            root_.Reset();
            pipeline_.Reset();
            heap_.Reset();
            cpuHeap_.Reset();
            scaled_.Reset();
            neutralDepth_.Reset();
        }
    }

    void clear(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, UINT slot, const float value[4])
    {
        transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto cpu = cpuHeap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += step_ * slot;
        device_->CreateUnorderedAccessView(resource, nullptr, nullptr, cpu);
        auto gpuCpu = heap_->GetCPUDescriptorHandleForHeapStart();
        gpuCpu.ptr += step_ * slot;
        device_->CopyDescriptorsSimple(1, gpuCpu, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto uav = heap_->GetGPUDescriptorHandleForHeapStart();
        uav.ptr += step_ * slot;
        ID3D12DescriptorHeap* heaps[]{heap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->ClearUnorderedAccessViewFloat(uav, cpu, resource, value, 0, nullptr);
        transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }

    void scale(ID3D12GraphicsCommandList* list)
    {
        auto srv = heap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateShaderResourceView(flow_.motion, nullptr, srv);
        auto uav = heap_->GetCPUDescriptorHandleForHeapStart();
        uav.ptr += step_;
        device_->CreateUnorderedAccessView(scaled_.Get(), nullptr, nullptr, uav);
        transition(list, flow_.motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(list, scaled_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(root_.Get());
        list->SetPipelineState(pipeline_.Get());
        ID3D12DescriptorHeap* heaps[]{heap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        const uint32_t constants[4]{std::bit_cast<uint32_t>(flow_.mvScaleX),
            std::bit_cast<uint32_t>(flow_.mvScaleY), render_.width, render_.height};
        list->SetComputeRoot32BitConstants(0, 4, constants, 0);
        auto table = heap_->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(1, table);
        table.ptr += step_;
        list->SetComputeRootDescriptorTable(2, table);
        list->Dispatch((render_.width + 7) / 8, (render_.height + 7) / 8, 1);
        transition(list, scaled_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        transition(list, flow_.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

public:
    SharedGuides(ID3D12Device* device, Extent render, std::unique_ptr<IAnrGuideProvider> local)
        : device_(device), render_(render), local_(std::move(local)) {}

    void configure(const ImportedFlow& flow) override
    {
        flow_ = flow;
        result_.reset();
        localPair_ = flow_routing(flow) != FlowRouting::Shared;
        if (flow_routing(flow) == FlowRouting::NrUnavailable) ++stats_.sharedUnavailable;
    }

    bool shared_path(ID3D12GraphicsCommandList* list, const FlowImage& image, bool reset,
        FlowProblem& problem) override
    {
        problem = FlowProblem::None;
        if (localPair_ || flow_routing(flow_) != FlowRouting::Shared) return false;
        ensure_pipeline();
        if (!pipeline_) {
            problem = FlowProblem::InvalidResource;
            return true; // Never fall through to local OFA while NR owns the pair.
        }
        const bool sameExtent = flow_.guideWidth == render_.width && flow_.guideHeight == render_.height;
        ID3D12Resource* motion = flow_.motion;
        const float zero[4]{0.0f, 0.0f, 0.0f, 0.0f};
        if (reset) {
            clear(list, scaled_.Get(), 1, zero);
            motion = scaled_.Get();
        } else if (!sameExtent) {
            scale(list);
            motion = scaled_.Get();
        }
        if (!depthReady_) {
            const float one[4]{1.0f, 1.0f, 1.0f, 1.0f};
            clear(list, neutralDepth_.Get(), 2, one);
            depthReady_ = true;
        }
        const NV_OF_FENCE_POINT ready = flow_.ready.fence ? NV_OF_FENCE_POINT{flow_.ready.fence, flow_.ready.value}
                                                         : NV_OF_FENCE_POINT{immediate_.Get(), 0};
        result_.emplace(FlowResult{image.key, motion, neutralDepth_.Get(), nullptr, nullptr, ready, reset,
            nullptr, nullptr, nullptr, {}, {}, {}, {}, {}, {}, nullptr, nullptr, {}});
        if (reset) ++stats_.sharedResets;
        else ++stats_.sharedHits;
        return true;
    }

    FlowProblem submit(const FlowInput& input) override
    {
        localPair_ = true;
        result_.reset();
        ++stats_.ofaExecutions;
        return local_->submit(input);
    }
    const FlowResult* result() const override { return localPair_ ? local_->result() : result_ ? &*result_ : nullptr; }
    bool collect(const JobKey& key, NV_OF_FENCE_POINT fence) override
    {
        if (!localPair_) return true;
        return local_->collect(key, fence);
    }
    bool close() override { return local_->close(); }
    SharedGuideStats stats() const override { return stats_; }
};
} // namespace

std::unique_ptr<IAnrGuideProvider> make_shared_anr_guides(ID3D12Device* device, Extent renderSize,
    uint64_t frameIntervalNs, std::unique_ptr<IAnrGuideProvider> local)
{
    if (!local) local = make_anr_guides(device, renderSize, frameIntervalNs);
    return std::make_unique<SharedGuides>(device, renderSize, std::move(local));
}

} // namespace nr::fg::vfg
