// SPDX-License-Identifier: GPL-3.0-only
#include "nr_stabilize.h"
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;

namespace {
void expect(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void checked(HRESULT hr, const char* why) { if (FAILED(hr)) throw std::runtime_error(why); }

constexpr uint32_t W = 16, H = 16;

ComPtr<ID3D12Resource> makeTex(ID3D12Device* dev, DXGI_FORMAT fmt, bool uav) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = W; d.Height = H; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    checked(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)), "tex");
    return r;
}

ComPtr<ID3D12Resource> makeBuf(ID3D12Device* dev, D3D12_HEAP_TYPE type, UINT64 bytes) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto st = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    ComPtr<ID3D12Resource> r;
    checked(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)), "buf");
    return r;
}

void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a; x.Transition.StateAfter = b;
    cmd->ResourceBarrier(1, &x);
}

struct RgbaF { float r, g, b, a; };

void uploadFp16(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex, ID3D12Resource* up, const std::vector<RgbaF>& px) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    const auto desc = tex->GetDesc();
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    void* p = nullptr;
    checked(up->Map(0, nullptr, &p), "map up");
    auto* base = static_cast<std::byte*>(p) + fp.Offset;
    for (uint32_t y = 0; y < H; ++y) {
        auto* dst = reinterpret_cast<HALF*>(base + size_t(y) * fp.Footprint.RowPitch);
        for (uint32_t x = 0; x < W; ++x) {
            const auto& c = px[size_t(y) * W + x];
            dst[x * 4 + 0] = XMConvertFloatToHalf(c.r);
            dst[x * 4 + 1] = XMConvertFloatToHalf(c.g);
            dst[x * 4 + 2] = XMConvertFloatToHalf(c.b);
            dst[x * 4 + 3] = XMConvertFloatToHalf(c.a);
        }
    }
    up->Unmap(0, nullptr);
    barrier(cmd, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dloc{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    D3D12_TEXTURE_COPY_LOCATION sloc{up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, fp};
    cmd->CopyTextureRegion(&dloc, 0, 0, 0, &sloc, nullptr);
    barrier(cmd, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void uploadMv(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex, ID3D12Resource* up, float dx, float dy,
    bool motionEdge) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    const auto desc = tex->GetDesc();
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    void* p = nullptr;
    checked(up->Map(0, nullptr, &p), "map mv");
    auto* base = static_cast<std::byte*>(p) + fp.Offset;
    for (uint32_t y = 0; y < H; ++y) {
        auto* dst = reinterpret_cast<float*>(base + size_t(y) * fp.Footprint.RowPitch);
        for (uint32_t x = 0; x < W; ++x) {
            dst[x * 2 + 0] = motionEdge && x == 7 && y == 8 ? dx + 4.0f : dx;
            dst[x * 2 + 1] = dy;
        }
    }
    up->Unmap(0, nullptr);
    barrier(cmd, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dloc{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    D3D12_TEXTURE_COPY_LOCATION sloc{up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, fp};
    cmd->CopyTextureRegion(&dloc, 0, 0, 0, &sloc, nullptr);
    barrier(cmd, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

std::vector<RgbaF> readbackFp16(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12CommandAllocator* alloc,
    ID3D12GraphicsCommandList* cmd, ID3D12Fence* fence, UINT64& fv, ID3D12Resource* tex, ID3D12Resource* rb) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    const auto desc = tex->GetDesc();
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    checked(alloc->Reset(), "rb alloc");
    checked(cmd->Reset(alloc, nullptr), "rb list");
    barrier(cmd, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dloc{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, fp};
    D3D12_TEXTURE_COPY_LOCATION sloc{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    cmd->CopyTextureRegion(&dloc, 0, 0, 0, &sloc, nullptr);
    barrier(cmd, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    checked(cmd->Close(), "rb close");
    ID3D12CommandList* ls[]{cmd};
    q->ExecuteCommandLists(1, ls);
    checked(q->Signal(fence, ++fv), "rb sig");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    checked(fence->SetEventOnCompletion(fv, ev), "rb ev");
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
    void* p = nullptr;
    checked(rb->Map(0, nullptr, &p), "map rb");
    const auto* base = static_cast<const std::byte*>(p) + fp.Offset;
    std::vector<RgbaF> out(size_t(W) * H);
    for (uint32_t y = 0; y < H; ++y) {
        const auto* src = reinterpret_cast<const HALF*>(base + size_t(y) * fp.Footprint.RowPitch);
        for (uint32_t x = 0; x < W; ++x) {
            out[size_t(y) * W + x] = {
                XMConvertHalfToFloat(src[x * 4 + 0]),
                XMConvertHalfToFloat(src[x * 4 + 1]),
                XMConvertHalfToFloat(src[x * 4 + 2]),
                XMConvertHalfToFloat(src[x * 4 + 3])
            };
        }
    }
    rb->Unmap(0, nullptr);
    return out;
}
}

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "expected nr_stabilize.dxil path");
        std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
        expect(bool(f) && f.tellg() > 0, "missing nr_stabilize.dxil");
        const auto sz = f.tellg();
        std::vector<std::byte> shader(static_cast<size_t>(sz));
        f.seekg(0); f.read(reinterpret_cast<char*>(shader.data()), sz);

        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> dev;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "warp");
        checked(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "device");
        ComPtr<ID3D12CommandQueue> q; D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        checked(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "queue");
        ComPtr<ID3D12CommandAllocator> alloc;
        checked(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
        ComPtr<ID3D12GraphicsCommandList> cmd;
        checked(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd)), "list");
        cmd->Close();
        ComPtr<ID3D12Fence> fence; checked(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        UINT64 fv = 0;

        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 9, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ComPtr<ID3D12DescriptorHeap> heap;
        checked(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "heap");

        nr::D3D12StabilizePass pass(dev.Get(), shader);

        auto curOrig = makeTex(dev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        auto curNr = makeTex(dev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        auto motion = makeTex(dev.Get(), DXGI_FORMAT_R32G32_FLOAT, false);
        auto prevOrig = makeTex(dev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        auto prevTemp = makeTex(dev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        auto prevHold = makeTex(dev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        auto outHold = makeTex(dev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        auto outTemp = makeTex(dev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        auto outOrig = makeTex(dev.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);

        const UINT64 upSize = 16ull * 1024;
        auto up0 = makeBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, upSize);
        auto up1 = makeBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, upSize);
        auto up2 = makeBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, upSize);
        auto up3 = makeBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, upSize);
        auto up4 = makeBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, upSize);
        auto up5 = makeBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, upSize);
        auto rb = makeBuf(dev.Get(), D3D12_HEAP_TYPE_READBACK, upSize);

        auto run = [&](const nr::StabilizeSettings& st, bool hasHist,
                       const std::vector<RgbaF>& cO, const std::vector<RgbaF>& cN,
                       float dx, float dy,
                       const std::vector<RgbaF>& pO, const std::vector<RgbaF>& pT, const std::vector<RgbaF>& pH,
                       bool motionEdge = false, bool readResidual = false) {
            checked(alloc->Reset(), "alloc");
            checked(cmd->Reset(alloc.Get(), nullptr), "list");
            uploadFp16(dev.Get(), cmd.Get(), curOrig.Get(), up0.Get(), cO);
            uploadFp16(dev.Get(), cmd.Get(), curNr.Get(), up1.Get(), cN);
            uploadMv(dev.Get(), cmd.Get(), motion.Get(), up2.Get(), dx, dy, motionEdge);
            uploadFp16(dev.Get(), cmd.Get(), prevOrig.Get(), up3.Get(), pO);
            uploadFp16(dev.Get(), cmd.Get(), prevTemp.Get(), up4.Get(), pT);
            uploadFp16(dev.Get(), cmd.Get(), prevHold.Get(), up5.Get(), pH);
            barrier(cmd.Get(), outHold.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            barrier(cmd.Get(), outTemp.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            barrier(cmd.Get(), outOrig.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            expect(pass.record(cmd.Get(), heap.Get(), {W, H}, st, hasHist,
                curOrig.Get(), curNr.Get(), motion.Get(),
                prevOrig.Get(), prevTemp.Get(), prevHold.Get(),
                outHold.Get(), outTemp.Get(), outOrig.Get()), "record failed");
            for (auto* r : {curOrig.Get(), curNr.Get(), motion.Get(), prevOrig.Get(), prevTemp.Get(), prevHold.Get()})
                barrier(cmd.Get(), r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            for (auto* r : {outHold.Get(), outTemp.Get(), outOrig.Get()})
                barrier(cmd.Get(), r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            checked(cmd->Close(), "close");
            ID3D12CommandList* ls[]{cmd.Get()}; q->ExecuteCommandLists(1, ls);
            checked(q->Signal(fence.Get(), ++fv), "sig");
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            checked(fence->SetEventOnCompletion(fv, ev), "ev");
            WaitForSingleObject(ev, INFINITE); CloseHandle(ev);
            return readbackFp16(dev.Get(), q.Get(), alloc.Get(), cmd.Get(), fence.Get(), fv,
                readResidual ? outTemp.Get() : outHold.Get(), rb.Get());
        };

        const std::vector<RgbaF> half(size_t(W) * H, {0.5f, 0.5f, 0.5f, 1.0f});
        const std::vector<RgbaF> nrSpike(size_t(W) * H, {0.7f, 0.7f, 0.7f, 1.0f});
        const std::vector<RgbaF> zero(size_t(W) * H, {0.0f, 0.0f, 0.0f, 1.0f});

        // 1. Cold/reset frame: output matches curNr untouched
        {
            const auto out = run({0.8f, 0.8f, 0.02f}, false, half, nrSpike, 0.f, 0.f, zero, zero, zero);
            expect(std::abs(out[0].r - 0.7f) < 0.002f, "reset passthrough");
        }
        // 2. NrHold suppresses static flicker (curOrig == prevOrig == 0.5, curNr = 0.7, prevHold = 0.5 -> 0.54)
        {
            const auto out = run({0.0f, 0.8f, 0.02f}, true, half, nrSpike, 0.f, 0.f, half, half, half);
            expect(std::abs(out[0].r - 0.54f) < 0.005f, "hold static dampening");
        }
        // 3. NrHold releases moving/changed pixels without ghosting (curOrig=0.9 vs prevOrig=0.1 -> raw 0.7)
        {
            const std::vector<RgbaF> bright(size_t(W) * H, {0.9f, 0.9f, 0.9f, 1.0f});
            const std::vector<RgbaF> dark(size_t(W) * H, {0.1f, 0.1f, 0.1f, 1.0f});
            const auto out = run({0.0f, 0.8f, 0.02f}, true, bright, nrSpike, 0.f, 0.f, dark, dark, dark);
            expect(std::abs(out[0].r - 0.7f) < 0.002f, "hold zero-ghosting on motion");
        }
        // 4. NrTemporal warps along NVOF motion (+2, +1) and suppresses shimmer inside 3x3 clamp
        {
            std::vector<RgbaF> cOrig(size_t(W) * H, {0.5f, 0.5f, 0.5f, 1.0f});
            std::vector<RgbaF> pOrig(size_t(W) * H, {0.5f, 0.5f, 0.5f, 1.0f});
            std::vector<RgbaF> pTemp(size_t(W) * H, {0.0f, 0.0f, 0.0f, 1.0f});
            std::vector<RgbaF> cNr(size_t(W) * H, {0.58f, 0.58f, 0.58f, 1.0f});
            // Put a 3x3 anchor so minNr includes 0.50 at (8,8) neighbor (7,8)
            cNr[8 * W + 7] = {0.50f, 0.50f, 0.50f, 1.0f};
            // Disable hold, enable temporal at 0.8 -> alpha = 0.68 -> 0.58*(1-0.68) + 0.50*0.68 = 0.5256
            const auto out = run({0.8f, 0.0f, 0.02f}, true, cOrig, cNr, -2.f, -1.f, pOrig, pTemp, pTemp);
            const auto v = out[8 * W + 8].r;
            expect(v > 0.515f && v < 0.540f, "temporal warped stabilization");
            const auto edge = run({0.8f, 0.0f, 0.02f}, true, cOrig, cNr, -2.f, -1.f, pOrig, pTemp, pTemp, true);
            expect(std::abs(edge[8 * W + 8].r - 0.58f) < 0.002f, "temporal must reject a motion discontinuity");
        }
        {
            const auto out = run({0.0f, 0.8f, 0.02f}, true, half, nrSpike, -2.f, -1.f, half, half, half);
            expect(std::abs(out[8 * W + 8].r - 0.7f) < 0.002f, "unwarped hold must release coherent motion");
        }
        // 5. NrTemporal rejects disocclusion when source warped luma mismatches curOrig
        {
            const std::vector<RgbaF> bright(size_t(W) * H, {0.9f, 0.9f, 0.9f, 1.0f});
            const std::vector<RgbaF> dark(size_t(W) * H, {0.1f, 0.1f, 0.1f, 1.0f});
            const auto out = run({0.8f, 0.0f, 0.02f}, true, bright, nrSpike, -2.f, -1.f, dark, dark, dark);
            expect(std::abs(out[8 * W + 8].r - 0.7f) < 0.002f, "temporal disocclusion rejection");
        }
        // Synthetic strong negative correction: test residual history, not vendor intensity.
        {
            const std::vector<RgbaF> bright(size_t(W) * H, {0.9f, 0.9f, 0.9f, 1.0f});
            const std::vector<RgbaF> dark(size_t(W) * H, {0.1f, 0.1f, 0.1f, 1.0f});
            const std::vector<RgbaF> strongResidual(size_t(W) * H, {-0.7f, -0.7f, -0.7f, 1.0f});
            auto currentNr=nrSpike;
            currentNr[8 * W + 7]=dark[0]; // allow the old correction through the 3x3 clamp
            const nr::StabilizeSettings settings{0.45f, 0.45f, 0.02f};
            const auto matched=run(settings, true, bright, currentNr, 0.f, 0.f,
                bright, strongResidual, dark);
            expect(matched[8 * W + 8].r < 0.6f, "strong residual influences matched history");
            const auto revealed=run(settings, true, bright, currentNr, 0.f, 0.f,
                dark, strongResidual, dark);
            expect(std::abs(revealed[8 * W + 8].r - 0.7f) < 0.002f,
                "disocclusion rejects strong temporal and hold history");
            const auto residual=run(settings, true, bright, currentNr, 0.f, 0.f,
                dark, strongResidual, dark, false, true);
            expect(std::abs(residual[8 * W + 8].r + 0.2f) < 0.002f,
                "disocclusion publishes current correction");
        }

        // Reset publishes a residual, not the NR color; hold must also be reflected in it.
        {
            const auto reset = run({0.8f, 0.8f, 0.02f}, false, half, nrSpike, 0.f, 0.f,
                zero, zero, zero, false, true);
            expect(std::abs(reset[8 * W + 8].r - 0.2f) < 0.002f, "reset residual");
            const auto held = run({0.0f, 0.8f, 0.02f}, true, half, nrSpike, 0.f, 0.f,
                half, zero, half, false, true);
            expect(std::abs(held[8 * W + 8].r - 0.04f) < 0.003f, "published hold residual");
            auto nextNr = nrSpike;
            nextNr[8 * W + 7] = half[0];
            const auto next = run({0.8f, 0.0f, 0.02f}, true, half, nextNr, 0.f, 0.f,
                half, held, half);
            expect(next[8 * W + 8].r > 0.58f && next[8 * W + 8].r < 0.60f,
                "next frame reuses published hold residual");
        }
        // A slightly brighter current source must not inherit the previous source's color.
        {
            const std::vector<RgbaF> shifted(size_t(W) * H, {0.505f, 0.505f, 0.505f, 1.0f});
            std::vector<RgbaF> current(size_t(W) * H, {0.585f, 0.585f, 0.585f, 1.0f});
            current[8 * W + 7] = shifted[0]; // admit a zero correction into the residual clamp
            const auto out = run({0.8f, 0.0f, 0.02f}, true, shifted, current, 0.f, 0.f,
                half, zero, half);
            expect(out[8 * W + 8].r > 0.525f && out[8 * W + 8].r < 0.536f,
                "temporal correction follows current source");
        }
        // Coherent subpixel flow is false motion when the stationary patch is a better match.
        {
            std::vector<RgbaF> source(size_t(W) * H, {0.2f, 0.2f, 0.2f, 1.0f});
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 8; x < W; ++x)
                    source[y * W + x] = {0.8f, 0.8f, 0.8f, 1.0f};
            auto current = source;
            current[8 * W + 7] = {0.1f, 0.1f, 0.1f, 1.0f};
            current[8 * W + 9] = {0.9f, 0.9f, 0.9f, 1.0f};
            std::vector<RgbaF> residual(size_t(W) * H, {-0.1f, -0.1f, -0.1f, 1.0f});
            residual[8 * W + 9] = {0.1f, 0.1f, 0.1f, 1.0f};
            const auto out = run({0.8f, 0.0f, 0.02f}, true, source, current, 0.75f, 0.f,
                source, residual, half);
            expect(out[8 * W + 8].r > 0.72f && out[8 * W + 8].r < 0.745f,
                "stationary patch cancels micro flow");
        }
        // Equal patch averages must not let hold bypass photometric rejection.
        {
            std::vector<RgbaF> current(size_t(W) * H), previous(size_t(W) * H);
            for (uint32_t y = 0; y < H; ++y) {
                for (uint32_t x = 0; x < W; ++x) {
                    float a = ((x + y) & 1) ? 0.2f : 0.8f;
                    float b = 1.0f - a;
                    current[y * W + x] = {a, a, a, 1.0f};
                    previous[y * W + x] = {b, b, b, 1.0f};
                }
            }
            const auto out = run({0.8f, 0.8f, 0.25f}, true, current, nrSpike, 0.f, 0.f,
                previous, zero, half);
            expect(std::abs(out[8 * W + 8].r - 0.7f) < 0.002f, "rejection also disables hold");
        }
        // A real translation must use the warped hypothesis and clamp old corrections.
        {
            std::vector<RgbaF> currentSource(size_t(W) * H), previousSource(size_t(W) * H);
            for (uint32_t y = 0; y < H; ++y) {
                for (uint32_t x = 0; x < W; ++x) {
                    float c = x < 8 ? 0.2f : 0.8f;
                    float p = x < 9 ? 0.2f : 0.8f;
                    currentSource[y * W + x] = {c, c, c, 1.0f};
                    previousSource[y * W + x] = {p, p, p, 1.0f};
                }
            }
            auto currentNr = currentSource;
            currentNr[8 * W + 8] = {0.9f, 0.9f, 0.9f, 1.0f};
            std::vector<RgbaF> oldResidual(size_t(W) * H, {-0.5f, -0.5f, -0.5f, 1.0f});
            const auto out = run({0.8f, 0.0f, 0.02f}, true, currentSource, currentNr, 1.f, 0.f,
                previousSource, oldResidual, half);
            expect(out[8 * W + 8].r > 0.825f && out[8 * W + 8].r < 0.84f,
                "warped match accepts and clamps residual");
        }

        std::puts("PASS stabilize_gpu: reset=ok hold=ok temporal=ok residual=ok micro_flow=ok rejection=ok");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
