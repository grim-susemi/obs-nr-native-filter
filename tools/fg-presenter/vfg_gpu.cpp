// SPDX-License-Identifier: GPL-3.0-only
#include "vfg_gpu.h"
#include <algorithm>
#include <dxgi1_5.h>
#include <intrin.h>
#include "vfg_content_compare_shader.h"

namespace nr::fg::vfg {
namespace {
D3D12_HEAP_PROPERTIES gpu_heap(D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CreationNodeMask = p.VisibleNodeMask = 1;
    return p;
}

constexpr size_t compare_bytes = 64 * sizeof(uint32_t); // Must match the HLSL shard count.

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
}
DescriptorStager::DescriptorStager(ID3D12Device* device) : device_(device)
{
    const D3D12_DESCRIPTOR_HEAP_DESC staging{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(device_->CreateDescriptorHeap(&staging, IID_PPV_ARGS(&cpu_)), "CPU descriptor heap");
}

void DescriptorStager::write_srv(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE dest) const
{
    const auto source = cpu_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateShaderResourceView(resource, desc, source);
    device_->CopyDescriptorsSimple(1, dest, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void DescriptorStager::write_uav(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE dest) const
{
    const auto source = cpu_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateUnorderedAccessView(resource, nullptr, nullptr, source);
    device_->CopyDescriptorsSimple(1, dest, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

Gpu::Gpu(const Session& s, HWND window, uint32_t multiplier)
    : window_(window), pushflow_(s.backend == Backend::PushFlow)
{
    ComPtr<IDXGIFactory5> factory5;
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    BOOL tearingSupport = FALSE;
    if (SUCCEEDED(factory.As(&factory5))) {
        (void)factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearingSupport, sizeof(tearingSupport));
    }
    allowTearing_ = tearingSupport == TRUE;
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapterByLuid(s.adapter, IID_PPV_ARGS(&adapter)), "OBS adapter LUID");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC q{};
    check(device_->CreateCommandQueue(&q, IID_PPV_ARGS(&ipcQueue)), "IPC queue");
    D3D12_COMMAND_QUEUE_DESC dq{};
    dq.Priority = s.gpuPriority ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    check(device_->CreateCommandQueue(&dq, IID_PPV_ARGS(&drawQueue)), "present queue");
    for (auto& slot : ring_) {
        check(device_->CreateCommandAllocator(dq.Type, IID_PPV_ARGS(&slot.alloc)), "present allocator");
        check(device_->CreateCommandList(0, dq.Type, slot.alloc.Get(), nullptr, IID_PPV_ARGS(&slot.list)), "present list");
        check(slot.list->Close(), "initial list close");
    }
    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_)), "present fence");
    event_.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_.h) check(E_FAIL, "present fence event");

    DXGI_SWAP_CHAIN_DESC1 swap{};
    swap.Width = s.width; swap.Height = s.height;
    swap.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap.SampleDesc.Count = 1;
    swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.BufferCount = 3;
    swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap.Scaling = DXGI_SCALING_STRETCH;
    swap.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    ComPtr<IDXGISwapChain1> base;
    check(factory->CreateSwapChainForHwnd(drawQueue.Get(), window, &swap, nullptr, nullptr, &base), "flip swapchain");
    check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "DXGI association");
    check(base.As(&swap_), "swapchain3");
    check(swap_->SetMaximumFrameLatency(1), "present latency");
    latency_.h = swap_->GetFrameLatencyWaitableObject();
    if (!latency_.h) check(E_FAIL, "frame latency event");

    desc_.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc_.Width = s.width; desc_.Height = s.height;
    desc_.DepthOrArraySize = desc_.MipLevels = 1;
    desc_.Format = swap.Format; desc_.SampleDesc.Count = 1;
    const auto heap = gpu_heap();
    for (auto& t : input)
        check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc_,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t)), "private IPC input");
    if (s.splitCompare) {
        for (auto& t : before)
            check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc_,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t)), "private before input");
        D3D12_RESOURCE_DESC upload{};
        upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload.Width = UINT64{D3D12_TEXTURE_DATA_PITCH_ALIGNMENT} * s.height;
        upload.Height = upload.DepthOrArraySize = upload.MipLevels = 1;
        upload.SampleDesc.Count = 1;
        upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto up = gpu_heap(D3D12_HEAP_TYPE_UPLOAD);
        check(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &upload,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&dividerUpload_)), "divider upload");
        dividerFp_.Footprint = {desc_.Format, 3, s.height, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        void* mapped{};
        const D3D12_RANGE noRead{0, 0};
        check(dividerUpload_->Map(0, &noRead, &mapped), "divider map");
        for (uint32_t y = 0; y < s.height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped) +
                size_t{y} * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            row[0] = row[1] = row[2] = 0xff00ff00u;
        }
        dividerUpload_->Unmap(0, nullptr);
    }
    if (s.gpuShotChange) shotChange_ = std::make_unique<VfgShotChange>(device_.Get());
    if (multiplier <= 1) return;
    auto syntheticDesc = desc_;
    syntheticDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (auto& t : syntheticTex)
        check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &syntheticDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t)), "synthetic texture");
    init_compare();
}

void Gpu::init_compare()
{
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    views.NumDescriptors = content_input_count;
    views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device_->CreateDescriptorHeap(&views, IID_PPV_ARGS(&compareViews_)), "content compare descriptor heap");
    compareViewStride_ = device_->GetDescriptorHandleIncrementSize(views.Type);
    auto handle = compareViews_->GetCPUDescriptorHandleForHeapStart();
    const DescriptorStager stager(device_.Get());
    for (const auto& texture : input) {
        stager.write_srv(texture.Get(), nullptr, handle);
        handle.ptr += compareViewStride_;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    D3D12_ROOT_PARAMETER parameters[4]{};
    for (UINT i = 0; i < 2; ++i) {
        ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0, 0};
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = {1, &ranges[i]};
    }
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor = {0, 0};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants = {0, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 4;
    signature.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr),
        "content compare root signature serialize");
    check(device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&compareRoot_)), "content compare root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = compareRoot_.Get();
    pipeline.CS = {vfg_content_shader, sizeof(vfg_content_shader)};
    check(device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&comparePipeline_)), "content compare pipeline");

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = compare_bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto allocate = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES initial, ComPtr<ID3D12Resource>& out) {
        const auto heap = gpu_heap(type);
        check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, initial, nullptr,
            IID_PPV_ARGS(&out)), "content compare buffer");
    };
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    allocate(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, compareFlags_);
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    allocate(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, compareZero_);
    buffer.Width = compare_bytes + VfgShotChange::slot_count * VfgShotChange::slot_bytes;
    allocate(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, compareReadback_);
    void* mapped{};
    const D3D12_RANGE noRead{0, 0}, written{0, compare_bytes};
    check(compareZero_->Map(0, &noRead, &mapped), "content compare zero buffer map");
    memset(mapped, 0, compare_bytes);
    compareZero_->Unmap(0, &written);
}

void Gpu::prepare_input(unsigned index)
{
    const uint64_t reader = inputReadFence_[index];
    if (reader <= inputWaitFence_[index]) return;
    const uint64_t completed = done_->GetCompletedValue();
    if (completed == UINT64_MAX) check(device_->GetDeviceRemovedReason(), "present device removed");
    if (completed < reader)
        check(ipcQueue->Wait(done_.Get(), reader), "private input recycle wait");
    inputWaitFence_[index] = reader;
}

FgFencePoint Gpu::signal_flow_ready()
{
    const auto value = ++fence_;
    check(drawQueue->Signal(done_.Get(), value), "imported flow ready signal");
    return {done_.Get(), value};
}

bool Gpu::commands_ready() const
{
    const auto completed = done_->GetCompletedValue();
    if (completed == UINT64_MAX) check(device_->GetDeviceRemovedReason(), "present device removed");
    return completed != UINT64_MAX && completed >= ring_[ringIdx_].fenceVal;
}

bool Gpu::begin_compare(unsigned previous, unsigned current, HANDLE ready, uint32_t dedupMode,
    uint32_t shotParity)
{
    if (compareFence_ || !commands_ready()) return false;
    if (done_->GetCompletedValue() < discardedCompareFence_) return false;
    if (!compareRoot_) init_compare();
    const uint32_t shotSlot = shotParity == UINT32_MAX ? VfgShotChange::transient_slot : shotParity;
    if (shotChange_ && shotSlot >= VfgShotChange::slot_count) return false;
    if (shotChange_ && done_->GetCompletedValue() < shotSlotFence_[shotSlot]) return false;
    auto* cmd = begin_available();
    auto flagsToCopy = transition(compareFlags_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->ResourceBarrier(1, &flagsToCopy);
    cmd->CopyBufferRegion(compareFlags_.Get(), 0, compareZero_.Get(), 0, compare_bytes);
    D3D12_RESOURCE_BARRIER barriers[]{
        transition(input[previous].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(input[current].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(compareFlags_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    cmd->ResourceBarrier(3, barriers);
    ID3D12DescriptorHeap* heaps[]{compareViews_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(compareRoot_.Get());
    cmd->SetPipelineState(comparePipeline_.Get());
    const auto base = compareViews_->GetGPUDescriptorHandleForHeapStart().ptr;
    D3D12_GPU_DESCRIPTOR_HANDLE previousView{};
    previousView.ptr = base + UINT64{compareViewStride_} * previous;
    D3D12_GPU_DESCRIPTOR_HANDLE currentView{};
    currentView.ptr = base + UINT64{compareViewStride_} * current;
    cmd->SetComputeRootDescriptorTable(0, previousView);
    cmd->SetComputeRootDescriptorTable(1, currentView);
    cmd->SetComputeRootUnorderedAccessView(2, compareFlags_->GetGPUVirtualAddress());
    const UINT constants[]{dedupMode ? 1u : 0u, content_band_steps,
        content_over_budget(static_cast<unsigned>(desc_.Width), static_cast<unsigned>(desc_.Height)), 0};
    cmd->SetComputeRoot32BitConstants(3, 4, constants, 0);
    cmd->Dispatch(static_cast<UINT>((desc_.Width + 63) / 64), (desc_.Height + 15) / 16, 1);
    if (shotChange_) {
        compareParity_ = shotSlot;
        shotChange_->record(cmd, {previousView, currentView, static_cast<uint32_t>(desc_.Width), desc_.Height,
            compareParity_, 1}, compareReadback_.Get());
    }
    for (unsigned i = 0; i < 2; ++i)
        std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    barriers[2] = transition(compareFlags_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(3, barriers);
    cmd->CopyBufferRegion(compareReadback_.Get(), 0, compareFlags_.Get(), 0, compare_bytes);
    flagsToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    flagsToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmd->ResourceBarrier(1, &flagsToCopy);
    compareDedupMode_ = dedupMode ? 1u : 0u;
    compareFence_ = finish(false);
    if (shotChange_) shotSlotFence_[compareParity_] = compareFence_;
    mark_inputs_read(previous, current, compareFence_);
    check(done_->SetEventOnCompletion(compareFence_, ready), "content compare event");
    return true;
}

std::optional<Gpu::CompareResult> Gpu::compare_result()
{
    const auto completed = done_->GetCompletedValue();
    if (completed == UINT64_MAX) check(device_->GetDeviceRemovedReason(), "content compare device removed");
    if (!compareFence_ || completed == UINT64_MAX || completed < compareFence_) return {};
    void* mapped{};
    const D3D12_RANGE readRange{0, compare_bytes +
        (shotChange_ ? VfgShotChange::slot_count * VfgShotChange::slot_bytes : 0)}, noWrite{0, 0};
    check(compareReadback_->Map(0, &readRange, &mapped), "content compare result map");
    const auto* flags = static_cast<const uint32_t*>(mapped);
    const bool same = compareDedupMode_ == 0
        ? std::all_of(flags, flags + compare_bytes / sizeof(*flags), [](uint32_t x) { return x == 0; })
        : flags[0] <= content_over_budget(static_cast<unsigned>(desc_.Width),
            static_cast<unsigned>(desc_.Height));
    const auto* shot = flags + compare_bytes / sizeof(*flags);
    const CompareResult result{same,
        shotChange_ && shot[VfgShotChange::cut_word(compareParity_)] != 0,
        shotChange_ && shot[VfgShotChange::flash_word(compareParity_)] != 0, compareParity_};
    compareReadback_->Unmap(0, &noWrite);
    compareFence_ = 0;
    return result;
}

void Gpu::discard_compare() noexcept
{
    if (!compareFence_) return;
    discardedCompareFence_ = compareFence_;
    compareFence_ = 0;
}

ID3D12GraphicsCommandList* Gpu::begin()
{
    auto& slot = ring_[ringIdx_];
    if (slot.fenceVal) {
        const auto completed = done_->GetCompletedValue();
        if (completed == UINT64_MAX) check(device_->GetDeviceRemovedReason(), "present device removed");
        if (completed < slot.fenceVal) {
            check(done_->SetEventOnCompletion(slot.fenceVal, event_.h), "ring slot fence event");
            const auto wait = WaitForSingleObject(event_.h, waitMs_);
            if (!pushflow_ && wait != WAIT_OBJECT_0) {
                check(device_->GetDeviceRemovedReason(), "present device removed");
                ShowWindowAsync(window_, SW_HIDE);
                TerminateProcess(GetCurrentProcess(), 5);
                __fastfail(FAST_FAIL_FATAL_APP_EXIT);
            }
            // The owner must classify the close and drain without unwinding Gpu.
            check_ring_wait(wait, device_->GetDeviceRemovedReason());
        }
    }
    return begin_available();
}
ID3D12GraphicsCommandList* Gpu::begin_available()
{
    auto& slot = ring_[ringIdx_];
    check(device_->GetDeviceRemovedReason(), "present device removed");
    slot.hudUse.reset(); // A reusable allocator's previous submission has completed.
    check(slot.alloc->Reset(), "present allocator reset");
    check(slot.list->Reset(slot.alloc.Get(), nullptr), "present list reset");
    return slot.list.Get();
}
uint64_t Gpu::finish(bool sync)
{
    auto& slot = ring_[ringIdx_];
    check(slot.list->Close(), "present close");
    ID3D12CommandList* lists[]{slot.list.Get()};
    drawQueue->ExecuteCommandLists(1, lists);
    const auto sigFence = ++fence_;
    check(drawQueue->Signal(done_.Get(), sigFence), "present queue signal");
    slot.fenceVal = sigFence;
    ringIdx_ = (ringIdx_ + 1) % ring_slots;
    if (!pushflow_ && sync) {
        const char* operation = "present queue fence event";
        HRESULT hr = done_->SetEventOnCompletion(sigFence, event_.h);
        if (SUCCEEDED(hr) && WaitForSingleObject(event_.h, waitMs_) != WAIT_OBJECT_0) {
            operation = "present fence wait";
            hr = E_FAIL;
        }
        const auto completed = done_->GetCompletedValue();
        const auto removed = device_->GetDeviceRemovedReason();
        if (SUCCEEDED(removed) && (completed == UINT64_MAX || completed < sigFence)) {
            ShowWindowAsync(window_, SW_HIDE);
            TerminateProcess(GetCurrentProcess(), 5);
            __fastfail(FAST_FAIL_FATAL_APP_EXIT);
        }
        check(removed, "present device removed");
        check(hr, operation);
        return sigFence;
    }
    if (sync) {
        check(done_->SetEventOnCompletion(sigFence, event_.h), "present queue fence event");
        const auto wait = WaitForSingleObject(event_.h, waitMs_);
        check_ring_wait(wait, device_->GetDeviceRemovedReason());
        const auto completed = done_->GetCompletedValue();
        check(completed == UINT64_MAX || completed < sigFence ? E_FAIL : S_OK, "present fence incomplete");
    } else {
        check(device_->GetDeviceRemovedReason(), "present device removed");
    }
    return sigFence;
}
uint64_t Gpu::record_draw(const Draw& draw)
{
    if (!commands_ready()) return 0;
    draw(begin_available());
    return finish(false);
}
void Gpu::mark_inputs_read(unsigned previous, unsigned current, uint64_t fenceVal)
{
    inputReadFence_[previous] = std::max(inputReadFence_[previous], fenceVal);
    inputReadFence_[current] = std::max(inputReadFence_[current], fenceVal);
}
void Gpu::sync_hud_lock(bool enabled)
{
    if (!enabled) hudLock_.reset();
    if (std::none_of(ring_.begin(), ring_.end(), [](const auto& slot) { return bool(slot.hudUse); })) return;
    const auto completed = done_->GetCompletedValue();
    if (completed == UINT64_MAX) {
        check(device_->GetDeviceRemovedReason(), "present device removed");
        return;
    }
    for (auto& slot : ring_)
        if (slot.fenceVal <= completed) slot.hudUse.reset();
}

bool Gpu::drain_hud_on_unwind() const noexcept
{
    if (std::none_of(ring_.begin(), ring_.end(), [](const auto& slot) { return bool(slot.hudUse); })) return true;
    // ExecuteCommandLists precedes the regular signal. Mark the queue again rather
    // than trusting a slot fence that may not have been updated before the throw.
    ComPtr<ID3D12Fence> drain;
    Handle ready;
    if (SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain))) &&
        (ready.h = CreateEventW(nullptr, FALSE, FALSE, nullptr)) &&
        SUCCEEDED(drawQueue->Signal(drain.Get(), 1)) &&
        SUCCEEDED(drain->SetEventOnCompletion(1, ready.h)) &&
        WaitForSingleObject(ready.h, 240) == WAIT_OBJECT_0 && drain->GetCompletedValue() == 1)
        return true;
    return FAILED(device_->GetDeviceRemovedReason());
}

void Gpu::update_hud_lock(unsigned current, unsigned previous, bool reset)
{
    if (!hudLock_) {
        hudLock_ = std::make_shared<VfgHudLock>(device_.Get(), input, syntheticTex);
        reset = true;
    }
    const auto held = reset ? current : previous;
    hudLock_->update(begin(), current, held, reset);
    ring_[ringIdx_].hudUse = hudLock_;
    mark_inputs_read(held, current, finish(false));
}
void Gpu::set_hud(uint32_t pos, uint32_t inset, uint32_t w, uint32_t h, const uint32_t* pixels)
{
    hudPos_ = pos;
    hudInset_ = inset;
    if (!pos || !pixels || !w || !h || w > 512 || h > 96 || w + 2 * inset > desc_.Width || h + 2 * inset > desc_.Height) {
        hudW_ = hudH_ = 0;
        return;
    }
    if (!hudUpload_) {
        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 512ull * 96ull * 4ull;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&hudUpload_)), "HUD upload buffer");
    }
    const UINT rowPitch = (w * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    hudFp_ = {};
    hudFp_.Offset = 0;
    hudFp_.Footprint.Format = desc_.Format;
    hudFp_.Footprint.Width = w;
    hudFp_.Footprint.Height = h;
    hudFp_.Footprint.Depth = 1;
    hudFp_.Footprint.RowPitch = rowPitch;
    void* mapped = nullptr;
    const D3D12_RANGE empty{0, 0};
    check(hudUpload_->Map(0, &empty, &mapped), "HUD upload map");
    auto* dstBytes = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < h; ++y) {
        memcpy(dstBytes + size_t(y) * rowPitch, pixels + size_t(y) * w, size_t(w) * 4u);
    }
    hudUpload_->Unmap(0, nullptr);
    hudW_ = w;
    hudH_ = h;
}
UINT Gpu::present(ID3D12Resource* source, bool synthetic, int beforeIndex, uint32_t splitPercent, bool splitSwap)
{
    if (!latencyReady_ && WaitForSingleObject(latency_.h, 240) != WAIT_OBJECT_0)
        check(E_FAIL, "swapchain latency timeout");
    (void)begin();
    return present_ready(source, synthetic, beforeIndex, splitPercent, splitSwap);
}

std::optional<UINT> Gpu::try_present_synthetic(const SyntheticFrame& frame, uint64_t deadlineNs,
    int beforeIndex, uint32_t splitPercent, bool splitSwap)
{
    if (!commands_ready()) return {};
    const auto now = clock_ns();
    if (now >= deadlineNs) return {};
    if (!latencyReady_) {
        const auto budgetMs = static_cast<DWORD>(std::min<uint64_t>((deadlineNs - now) / 1'000'000, 240));
        const DWORD result = WaitForSingleObject(latency_.h, budgetMs);
        if (result == WAIT_TIMEOUT) return {};
        if (result != WAIT_OBJECT_0)
            check(result == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL, "synthetic swapchain wait");
        latencyReady_ = true;
    }
    // Keep any acquired latency signal for the next frame if we woke too late.
    if (clock_ns() >= deadlineNs) return {};
    auto* cmd = begin_available();
    auto* display = record_synthetic_display(cmd, frame);
    const auto id = present_ready(display, true, beforeIndex, splitPercent, splitSwap);
    if (shotChange_ && frame.shotCompared) shotSlotFence_[frame.shotParity] = fence_;
    mark_inputs_read(frame.prevIndex, frame.currIndex, fence_);
    return id;
}

ID3D12Resource* Gpu::record_synthetic_display(ID3D12GraphicsCommandList* cmd, const SyntheticFrame& frame)
{
    frame.resolve(cmd);
    auto* display = frame.texture;
    if (frame.hudLock) {
        hudLock_->compose(cmd, frame.slotParity * (max_multiplier - 1) + frame.subframe, frame.prevIndex);
        ring_[ringIdx_].hudUse = hudLock_; // Also protects scratch through cut copy and the present copy.
        display = hudLock_->output();
    }
    if (shotChange_ && frame.shotCompared)
        shotChange_->replace_on_cut(cmd, display, input[frame.currIndex].Get(), frame.shotParity);
    return display;
}

void Gpu::record_present_copy(ID3D12GraphicsCommandList* cmd, const PresentCopy& copy)
{
    const auto drawHud = [&] {
        const UINT dstX = hudPos_ == 2 ? static_cast<UINT>(desc_.Width) - hudW_ - hudInset_ : hudInset_;
        D3D12_TEXTURE_COPY_LOCATION dloc{copy.destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION sloc{hudUpload_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, hudFp_};
        D3D12_BOX box{0, 0, 0, hudW_, hudH_, 1};
        cmd->CopyTextureRegion(&dloc, dstX, hudInset_, 0, &sloc, &box);
    };
    const auto drawSplit = [&] {
        const UINT width = static_cast<UINT>(desc_.Width);
        const UINT split = static_cast<UINT>(UINT64{width} * std::min(copy.splitPercent, 100u) / 100);
        auto barrier = transition(before[copy.beforeIndex].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION d{copy.destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION s{before[copy.beforeIndex].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        if (!copy.splitSwap && split) {
            D3D12_BOX box{0, 0, 0, split, desc_.Height, 1};
            cmd->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
        } else if (copy.splitSwap && split < width) {
            D3D12_BOX box{split, 0, 0, width, desc_.Height, 1};
            cmd->CopyTextureRegion(&d, split, 0, 0, &s, &box);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        cmd->ResourceBarrier(1, &barrier);
        if (hudW_ && hudH_) drawHud();
        D3D12_TEXTURE_COPY_LOCATION green{dividerUpload_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, dividerFp_};
        cmd->CopyTextureRegion(&d, std::min(split, width - 3), 0, 0, &green, nullptr);
    };
    D3D12_RESOURCE_BARRIER b[]{
        transition(copy.destination, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
        transition(copy.source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    cmd->ResourceBarrier(2, b);
    cmd->CopyResource(copy.destination, copy.source);
    if (copy.beforeIndex >= 0) drawSplit();
    else if (hudW_ && hudH_) drawHud();
    for (auto& barrier : b) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(2, b);
}

UINT Gpu::present_ready(ID3D12Resource* source, bool synthetic, int beforeIndex, uint32_t splitPercent, bool splitSwap)
{
    latencyReady_ = false;
    ComPtr<ID3D12Resource> dst;
    check(swap_->GetBuffer(swap_->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&dst)), "back buffer");
    record_present_copy(ring_[ringIdx_].list.Get(), {source, dst.Get(), beforeIndex, splitPercent, splitSwap});
    const auto submitted = finish(false);
    if (!synthetic) {
        for (unsigned i = 0; i < input.size(); ++i)
            if (source == input[i].Get()) mark_inputs_read(i, i, submitted);
    }
    if (beforeIndex >= 0) {
        const auto index = static_cast<unsigned>(beforeIndex);
        mark_inputs_read(index, index, submitted);
    }
    const auto hr = allowTearing_ ? swap_->Present(0, DXGI_PRESENT_ALLOW_TEARING) : swap_->Present(1, 0);
    check(hr == S_OK ? hr : FAILED(hr) ? hr : E_FAIL, "DXGI Present");
    UINT presentId{};
    check(swap_->GetLastPresentCount(&presentId), "DXGI present count");
    return presentId;
}
} // namespace nr::fg::vfg
