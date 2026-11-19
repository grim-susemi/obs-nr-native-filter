// SPDX-License-Identifier: GPL-3.0-only
#include "nr_upscale.h"
#include <DirectXPackedVector.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;

namespace {
void expect(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void checked(HRESULT hr, const char* why) { if (FAILED(hr)) throw std::runtime_error(why); }

ComPtr<ID3D12Resource> makeTex(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
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

void uploadFp16(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex, ID3D12Resource* up,
    uint32_t w, uint32_t h, const std::vector<RgbaF>& px) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    const auto desc = tex->GetDesc();
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    void* p = nullptr; checked(up->Map(0, nullptr, &p), "map");
    auto* base = static_cast<std::byte*>(p) + fp.Offset;
    for (uint32_t y = 0; y < h; ++y) {
        auto* dst = reinterpret_cast<HALF*>(base + size_t(y) * fp.Footprint.RowPitch);
        for (uint32_t x = 0; x < w; ++x) {
            const auto& c = px[size_t(y) * w + x];
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

void uploadRgba8(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex, ID3D12Resource* up,
    uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    const auto desc = tex->GetDesc();
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    void* p = nullptr; checked(up->Map(0, nullptr, &p), "map8");
    auto* base = static_cast<std::byte*>(p) + fp.Offset;
    for (uint32_t y = 0; y < h; ++y) {
        auto* dst = reinterpret_cast<uint8_t*>(base + size_t(y) * fp.Footprint.RowPitch);
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 4 + 0] = r; dst[x * 4 + 1] = g; dst[x * 4 + 2] = b; dst[x * 4 + 3] = a;
        }
    }
    up->Unmap(0, nullptr);
    barrier(cmd, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dloc{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    D3D12_TEXTURE_COPY_LOCATION sloc{up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, fp};
    cmd->CopyTextureRegion(&dloc, 0, 0, 0, &sloc, nullptr);
    barrier(cmd, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

std::vector<uint8_t> readbackRgba8(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12CommandAllocator* alloc,
    ID3D12GraphicsCommandList* cmd, ID3D12Fence* fence, UINT64& fv, uint32_t w, uint32_t h, ID3D12Resource* tex, ID3D12Resource* rb) {
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
    WaitForSingleObject(ev, 5000); CloseHandle(ev);
    void* p = nullptr; checked(rb->Map(0, nullptr, &p), "rb map");
    const auto* base = static_cast<const std::byte*>(p) + fp.Offset;
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(&out[size_t(y) * w * 4], base + size_t(y) * fp.Footprint.RowPitch, size_t(w) * 4);
    rb->Unmap(0, nullptr);
    return out;
}
} // namespace

int main(int argc, char** argv) {
    expect(argc >= 2, "nr_cas.dxil path required");
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
    expect(bool(f), "open cas shader");
    const auto len = f.tellg();
    std::vector<std::byte> code(static_cast<size_t>(len));
    f.seekg(0); f.read(reinterpret_cast<char*>(code.data()), len);

    ComPtr<IDXGIFactory4> fac; checked(CreateDXGIFactory1(IID_PPV_ARGS(&fac)), "factory");
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{}; ad->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        ad.Reset();
    }
    if (!ad) checked(fac->EnumWarpAdapter(IID_PPV_ARGS(&ad)), "warp");
    ComPtr<ID3D12Device> dev; checked(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "dev");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> q; checked(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "q");
    ComPtr<ID3D12CommandAllocator> alloc; checked(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    ComPtr<ID3D12GraphicsCommandList> cmd; checked(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd)), "cmd");
    checked(cmd->Close(), "close");
    ComPtr<ID3D12Fence> fence; checked(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    UINT64 fv = 0;

    auto makeHeap = [&]() {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ComPtr<ID3D12DescriptorHeap> h; checked(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&h)), "heap");
        return h;
    };
    auto hPrep = makeHeap(), hOut = makeHeap();

    constexpr uint32_t SW = 16, SH = 16, OW = 32, OH = 32;
    auto srcTex = makeTex(dev.Get(), SW, SH, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    auto origTex = makeTex(dev.Get(), SW, SH, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto midTex = makeTex(dev.Get(), OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    auto outTex = makeTex(dev.Get(), OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    auto upSrc = makeBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 1024 * 64);
    auto upOrig = makeBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 1024 * 64);
    auto rb = makeBuf(dev.Get(), D3D12_HEAP_TYPE_READBACK, 1024 * 256);

    // Step 1: 16x16 sRGB edge pattern -> 32x32 linear -> 32x32 CAS+encode RGBA8
    std::vector<RgbaF> px(SW * SH);
    for (uint32_t y = 0; y < SH; ++y)
        for (uint32_t x = 0; x < SW; ++x)
            px[y * SW + x] = (x >= 6 && x < 10 && y >= 6 && y < 10)
                ? RgbaF{0.8f, 0.8f, 0.8f, 1.0f}
                : RgbaF{0.2f, 0.2f, 0.2f, 1.0f};

    nr::D3D12CasPass pass(dev.Get(), code);
    checked(alloc->Reset(), "r alloc");
    checked(cmd->Reset(alloc.Get(), nullptr), "r cmd");
    uploadFp16(dev.Get(), cmd.Get(), srcTex.Get(), upSrc.Get(), SW, SH, px);
    uploadRgba8(dev.Get(), cmd.Get(), origTex.Get(), upOrig.Get(), SW, SH, 50, 50, 50, 255);
    barrier(cmd.Get(), midTex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    expect(pass.record(cmd.Get(), hPrep.Get(), {0u, SW, SH, OW, OH, 1u, 3u, 1u, 0.0f, 1.0f},
        srcTex.Get(), nullptr, midTex.Get()), "cas prep resize");
    barrier(cmd.Get(), midTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(cmd.Get(), outTex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    expect(pass.record(cmd.Get(), hOut.Get(), {1u, SW, SH, OW, OH, 1u, 3u, 0u, 0.8f, 1.0f},
        midTex.Get(), origTex.Get(), outTex.Get()), "cas encode");
    barrier(cmd.Get(), midTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(cmd.Get(), outTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    barrier(cmd.Get(), srcTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(cmd.Get(), origTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    checked(cmd->Close(), "r close");
    ID3D12CommandList* ls[]{cmd.Get()};
    q->ExecuteCommandLists(1, ls);
    checked(q->Signal(fence.Get(), ++fv), "sig");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    checked(fence->SetEventOnCompletion(fv, ev), "ev");
    WaitForSingleObject(ev, 5000); CloseHandle(ev);

    const auto out = readbackRgba8(dev.Get(), q.Get(), alloc.Get(), cmd.Get(), fence.Get(), fv, OW, OH, outTex.Get(), rb.Get());
    // Flat corner (1,1) should stay ~0.2*255 = 51, alpha 255
    const size_t flatIdx = (1 * OW + 1) * 4;
    expect(std::abs(int(out[flatIdx]) - 51) <= 3 && out[flatIdx + 3] == 255, "flat region preserved");
    // Center bright square (16,16) should be bright ~204
    const size_t centerIdx = (16 * OW + 16) * 4;
    expect(out[centerIdx] >= 195, "center bright preserved");
    // Check contract extent helpers
    expect(nr::upscale_extent({1920, 1080}, 0) == (nr::Extent{1920, 1080}), "off extent");
    expect(nr::upscale_extent({1920, 1080}, 1) == (nr::Extent{2560, 1440}), "1440p extent");
    expect(nr::upscale_extent({1920, 1080}, 2) == (nr::Extent{3840, 2160}), "4k extent");
    expect(nr::upscale_extent({1920, 1080}, 3) == (nr::Extent{1920, 1080}), "dlaa extent");
    std::printf("PASS upscale_cas_tests corner=%u center=%u\n", out[flatIdx], out[centerIdx]);
    if (argc >= 3) {
        const std::filesystem::path srDir = std::filesystem::canonical(argv[2]);
        const auto binding = nr::runtime::resolve_driver();
        expect(binding.signatureVerified, "verified NVIDIA driver");
        HMODULE drv = LoadLibraryExW(binding.identity.canonicalPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        expect(drv != nullptr, "load _nvngx.dll");
        using InitFn = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
        using ShutFn = NVSDK_NGX_Result (*)(ID3D12Device*, unsigned int&);
        auto initFn = reinterpret_cast<InitFn>(GetProcAddress(drv, "NVSDK_NGX_D3D12_Init_Ext"));
        auto shutFn = reinterpret_cast<ShutFn>(GetProcAddress(drv, "NVSDK_NGX_D3D12_Shutdown1"));
        auto allocFn = reinterpret_cast<decltype(&NVSDK_NGX_D3D12_AllocateParameters)>(GetProcAddress(drv, "NVSDK_NGX_D3D12_AllocateParameters"));
        nr::runtime::Dispatch d{
            allocFn ? allocFn : reinterpret_cast<decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters)>(GetProcAddress(drv, "NVSDK_NGX_D3D12_GetCapabilityParameters")),
            reinterpret_cast<decltype(&NVSDK_NGX_D3D12_DestroyParameters)>(GetProcAddress(drv, "NVSDK_NGX_D3D12_DestroyParameters")),
            reinterpret_cast<decltype(&NVSDK_NGX_D3D12_CreateFeature)>(GetProcAddress(drv, "NVSDK_NGX_D3D12_CreateFeature")),
            reinterpret_cast<decltype(&NVSDK_NGX_D3D12_EvaluateFeature)>(GetProcAddress(drv, "NVSDK_NGX_D3D12_EvaluateFeature")),
            reinterpret_cast<decltype(&NVSDK_NGX_D3D12_ReleaseFeature)>(GetProcAddress(drv, "NVSDK_NGX_D3D12_ReleaseFeature"))
        };
        const auto srW = srDir.wstring();
        const auto dataDir = std::filesystem::temp_directory_path() / L"obs-nr-dlss-sr-smoke";
        std::filesystem::create_directories(dataDir);
        const auto dataW = dataDir.wstring();
        const wchar_t* paths[]{ dataW.c_str(), srW.c_str() };
        NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path = paths; info.PathListInfo.Length = 2;
        const auto initRes = initFn(0x24480451ull, dataW.c_str(), dev.Get(), NVSDK_NGX_Version_API, &info);
        expect(initRes == NVSDK_NGX_Result_Success, "NGX Init_Ext");
        for (const auto& [outW, outH, tag] : std::array<std::tuple<uint32_t, uint32_t, const char*>, 2>{
            {{2560, 1440, "1440p"}, {3840, 2160, "4K"}}}) {
            constexpr uint32_t IW = 1920, IH = 1080;
            auto srIn = makeTex(dev.Get(), IW, IH, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
            auto srDepth = makeTex(dev.Get(), IW, IH, DXGI_FORMAT_R32_FLOAT, false);
            auto srMv = makeTex(dev.Get(), IW, IH, DXGI_FORMAT_R16G16_FLOAT, false);
            auto srOut = makeTex(dev.Get(), outW, outH, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
            nr::D3D12DlssSr sr;
            checked(alloc->Reset(), "sr c alloc");
            checked(cmd->Reset(alloc.Get(), nullptr), "sr c cmd");
            const bool cOk = sr.create(d, cmd.Get(), {IW, IH}, {outW, outH}, 1u, false);
            checked(cmd->Close(), "sr c close");
            ID3D12CommandList* cl[]{ cmd.Get() }; q->ExecuteCommandLists(1, cl);
            checked(q->Signal(fence.Get(), ++fv), "sr c sig");
            HANDLE ev1 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            checked(fence->SetEventOnCompletion(fv, ev1), "sr c ev");
            WaitForSingleObject(ev1, 5000); CloseHandle(ev1);
            expect(cOk && sr.created(), "D3D12DlssSr::create");

            checked(alloc->Reset(), "sr e alloc");
            checked(cmd->Reset(alloc.Get(), nullptr), "sr e cmd");
            barrier(cmd.Get(), srIn.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(cmd.Get(), srDepth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(cmd.Get(), srMv.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(cmd.Get(), srOut.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const bool eOk = sr.evaluate(cmd.Get(), srIn.Get(), srOut.Get(), srDepth.Get(), srMv.Get(), true);
            barrier(cmd.Get(), srOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            checked(cmd->Close(), "sr e close");
            q->ExecuteCommandLists(1, cl);
            checked(q->Signal(fence.Get(), ++fv), "sr e sig");
            HANDLE ev2 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            checked(fence->SetEventOnCompletion(fv, ev2), "sr e ev");
            WaitForSingleObject(ev2, 5000); CloseHandle(ev2);
            expect(eOk, "D3D12DlssSr::evaluate");
            expect(sr.release(), "D3D12DlssSr::release");
            std::printf("PASS dlss_sr_gpu %s 1920x1080->%ux%u model=M\n", tag, outW, outH);
        }
        unsigned int rem = 0;
        if (shutFn) (void)shutFn(dev.Get(), rem);
        FreeLibrary(drv);
    }
    return 0;
}
