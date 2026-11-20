// SPDX-License-Identifier: GPL-3.0-only
#include "vfg_gpu.h"
#include <algorithm>
#include <dxgi1_5.h>
#include <intrin.h>

namespace nr::fg::vfg {
namespace {
D3D12_HEAP_PROPERTIES gpu_heap()
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = D3D12_HEAP_TYPE_DEFAULT;
    p.CreationNodeMask = p.VisibleNodeMask = 1;
    return p;
}
}
Gpu::Gpu(const Session& s, HWND window, uint32_t multiplier) : window_(window)
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
    if (multiplier <= 1) return;
    outputCount = std::clamp<uint32_t>(multiplier, 2, max_multiplier) - 1;
    device_->GetCopyableFootprints(&desc_, 0, 1, 0, &footprint_, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    allocated = device_->GetResourceAllocationInfo(0, 1, &buffer).SizeInBytes;
    const size_t totalExchange = 2 + outputCount;
    for (size_t i = 0; i < totalExchange; ++i) {
        check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&exchange[i])), "CUDA exchange buffer");
        check(device_->CreateSharedHandle(exchange[i].Get(), nullptr, GENERIC_ALL, nullptr, &exported[i].h), "CUDA NT handle");
    }
}

ID3D12GraphicsCommandList* Gpu::begin()
{
    auto& slot = ring_[ringIdx_];
    if (slot.fenceVal) {
        const auto completed = done_->GetCompletedValue();
        if (completed == UINT64_MAX) check(device_->GetDeviceRemovedReason(), "present device removed");
        if (completed < slot.fenceVal) {
            check(done_->SetEventOnCompletion(slot.fenceVal, event_.h), "ring slot fence event");
            if (WaitForSingleObject(event_.h, 240) != WAIT_OBJECT_0) {
                ShowWindowAsync(window_, SW_HIDE);
                TerminateProcess(GetCurrentProcess(), 5);
                __fastfail(FAST_FAIL_FATAL_APP_EXIT);
            }
        }
    }
    check(slot.alloc->Reset(), "present allocator reset");
    check(slot.list->Reset(slot.alloc.Get(), nullptr), "present list reset");
    return slot.list.Get();
}
void Gpu::finish(bool sync)
{
    auto& slot = ring_[ringIdx_];
    check(slot.list->Close(), "present close");
    ID3D12CommandList* lists[]{slot.list.Get()};
    drawQueue->ExecuteCommandLists(1, lists);
    const auto sigFence = ++fence_;
    check(drawQueue->Signal(done_.Get(), sigFence), "present queue signal");
    slot.fenceVal = sigFence;
    ringIdx_ = (ringIdx_ + 1) % ring_slots;
    if (!sync) {
        const auto removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) check(removed, "present device removed");
        return;
    }
    const char* operation = "present queue fence event";
    HRESULT hr = done_->SetEventOnCompletion(sigFence, event_.h);
    if (SUCCEEDED(hr) && WaitForSingleObject(event_.h, 240) != WAIT_OBJECT_0) {
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
}
void Gpu::move(ID3D12Resource* texture, ID3D12Resource* buffer, bool toBuffer)
{
    auto* cmd = ring_[ringIdx_].list.Get();
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    auto& t = b.Transition;
    t = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
        toBuffer ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST};
    cmd->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION tex{}, buf{};
    tex.pResource = texture;
    tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    buf.pResource = buffer;
    buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buf.PlacedFootprint = footprint_;
    cmd->CopyTextureRegion(toBuffer ? &buf : &tex, 0, 0, 0, toBuffer ? &tex : &buf, nullptr);
    std::swap(t.StateBefore, t.StateAfter);
    cmd->ResourceBarrier(1, &b);
}
void Gpu::stage(unsigned previous, unsigned current)
{
    (void)begin();
    move(input[previous].Get(), exchange[0].Get(), true);
    move(input[current].Get(), exchange[1].Get(), true);
    finish(true);
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
UINT Gpu::present(ID3D12Resource* source, bool synthetic)
{
    if (WaitForSingleObject(latency_.h, 240) != WAIT_OBJECT_0) check(E_FAIL, "swapchain latency timeout");
    ComPtr<ID3D12Resource> dst;
    check(swap_->GetBuffer(swap_->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&dst)), "back buffer");
    auto* cmd = begin();
    const auto drawHud = [&] {
        const UINT dstX = hudPos_ == 2 ? static_cast<UINT>(desc_.Width) - hudW_ - hudInset_ : hudInset_;
        D3D12_TEXTURE_COPY_LOCATION dloc{dst.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
        D3D12_TEXTURE_COPY_LOCATION sloc{hudUpload_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, hudFp_};
        D3D12_BOX box{0, 0, 0, hudW_, hudH_, 1};
        cmd->CopyTextureRegion(&dloc, dstX, hudInset_, 0, &sloc, &box);
    };
    if (synthetic) {
        move(dst.Get(), source, false);
        if (hudW_ && hudH_) {
            D3D12_RESOURCE_BARRIER hb{};
            hb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            hb.Transition = {dst.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST};
            cmd->ResourceBarrier(1, &hb);
            drawHud();
            std::swap(hb.Transition.StateBefore, hb.Transition.StateAfter);
            cmd->ResourceBarrier(1, &hb);
        }
    } else {
        D3D12_RESOURCE_BARRIER b[2]{};
        b[0].Transition = {dst.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST};
        b[1].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(2, b);
        cmd->CopyResource(dst.Get(), source);
        if (hudW_ && hudH_) drawHud();
        for (auto& barrier : b) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        cmd->ResourceBarrier(2, b);
    }
    finish(false);
    const auto hr = allowTearing_ ? swap_->Present(0, DXGI_PRESENT_ALLOW_TEARING) : swap_->Present(1, 0);
    check(hr == S_OK ? hr : FAILED(hr) ? hr : E_FAIL, "DXGI Present");
    UINT presentId{};
    check(swap_->GetLastPresentCount(&presentId), "DXGI present count");
    return presentId;
}
} // namespace nr::fg::vfg
