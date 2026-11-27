// SPDX-License-Identifier: GPL-3.0-only
#include "anr_guides.h"
#include "anr_d3d.h"
#include "anr_bgra_vs.h"
#include "anr_bgra_ps.h"
#include "vfg_gpu.h"
#include <filesystem>
#include <fstream>

namespace nr::fg::vfg {
namespace {
using Microsoft::WRL::ComPtr;

class Guides final : public IAnrGuideProvider {
    D3D12Guides inner_;
public:
    Guides(ID3D12Device* device, ID3D12CommandQueue* queue, Extent size, std::span<const std::byte> shader,
        uint64_t frameIntervalNs)
        : inner_(device, queue, size, shader, frameIntervalNs) {}
    FlowProblem submit(const FlowInput& input) override { return inner_.submit(input); }
    const FlowResult* result() const override { return inner_.result(); }
    bool collect(const JobKey& key, NV_OF_FENCE_POINT fence) override { return inner_.collect(key, fence); }
    bool close() override { return inner_.close(); }
};
} // namespace

std::unique_ptr<IAnrGuideProvider> make_anr_guides(ID3D12Device* device, Extent size, uint64_t frameIntervalNs)
{
    std::wstring path(32768, L'\0');
    DWORD count = GetEnvironmentVariableW(L"OBS_NR_GUIDES_SHADER", path.data(), static_cast<DWORD>(path.size()));
    if (count >= path.size()) throw std::runtime_error("ANR guide shader path too long");
    if (count) {
        path.resize(count);
    } else {
        count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!count || count >= path.size()) throw std::runtime_error("ANR executable path unavailable");
        path.resize(count);
        path = (std::filesystem::path(path).parent_path() / L"nr_guides.cso").native();
    }
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (length <= 0 || length > 1024 * 1024) throw std::runtime_error("ANR guide shader unavailable");
    std::vector<std::byte> shader(static_cast<size_t>(length));
    if (!file.seekg(0).read(reinterpret_cast<char*>(shader.data()), static_cast<std::streamsize>(shader.size())))
        throw std::runtime_error("ANR guide shader read failed");
    ComPtr<ID3D12CommandQueue> queue;
    const D3D12_COMMAND_QUEUE_DESC desc{};
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "ANR guide queue");
    return std::make_unique<Guides>(device, queue.Get(), size, shader, guide_interval_ns(frameIntervalNs));
}


AnrBgra::AnrBgra(ID3D12Device* device, Extent size) : size_(size), stager_(device)
{
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC signature{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> bytes;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, nullptr),
        "ANR BGRA root signature");
    check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
        IID_PPV_ARGS(&root_)), "ANR BGRA root object");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root_.Get();
    state.VS = {anr_bgra_vs, sizeof(anr_bgra_vs)};
    state.PS = {anr_bgra_ps, sizeof(anr_bgra_ps)};
    state.BlendState.RenderTarget[0] = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    state.SampleMask = UINT_MAX;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = 1;
    state.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    state.SampleDesc.Count = 1;
    check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&pipeline_)), "ANR BGRA pipeline");
    const D3D12_DESCRIPTOR_HEAP_DESC srv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srvs_)), "ANR BGRA SRVs");
    check(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvs_)), "ANR BGRA RTVs");
    srvStep_ = device->GetDescriptorHandleIncrementSize(srv.Type);
    rtvStep_ = device->GetDescriptorHandleIncrementSize(rtv.Type);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size.width;
    desc.Height = size.height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = state.RTVFormats[0];
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto handle = rtvs_->GetCPUDescriptorHandleForHeapStart();
    for (auto& texture : textures_) {
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&texture)), "ANR BGRA texture");
        device->CreateRenderTargetView(texture.Get(), nullptr, handle);
        handle.ptr += rtvStep_;
    }
}

void AnrBgra::record(ID3D12GraphicsCommandList* list, ID3D12Resource* input, unsigned slot)
{
    auto srv = srvs_->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += slot * srvStep_;
    stager_.write_srv(input, nullptr, srv);
    auto table = srvs_->GetGPUDescriptorHandleForHeapStart();
    table.ptr += slot * srvStep_;
    auto rtv = rtvs_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += slot * rtvStep_;
    auto* output = textures_[slot].Get();
    transition(list, input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(size_.width), static_cast<float>(size_.height), 0, 1};
    const D3D12_RECT rect{0, 0, static_cast<LONG>(size_.width), static_cast<LONG>(size_.height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &rect);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(root_.Get());
    list->SetPipelineState(pipeline_.Get());
    ID3D12DescriptorHeap* heaps[]{srvs_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(0, table);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    transition(list, input, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
}
} // namespace nr::fg::vfg
