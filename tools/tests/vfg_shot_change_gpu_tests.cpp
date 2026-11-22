// SPDX-License-Identifier: GPL-3.0-only
#include "../fg-presenter/vfg_shot_change.h"
#include "../fg-presenter/vfg_gpu.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using nr::fg::vfg::VfgShotChange;
namespace {
constexpr UINT width = 64, height = 36, pitch = width * 4, imageBytes = pitch * height;
using Image = std::array<uint32_t, width * height>;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT hr) { require(SUCCEEDED(hr), "D3D12 fixture operation failed"); }
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 value{};
    VfgShotChange shot;
    static ComPtr<ID3D12Device> create_device()
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> result;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&result)));
        return result;
    }
    Fixture() : device(create_device()), shot(device.Get())
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&cmd)));
        check(cmd->Close());
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "fence event");
    }
    ~Fixture() { if (event) CloseHandle(event); }
    ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&result)));
        return result;
    }
    ComPtr<ID3D12Resource> texture()
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&result)));
        return result;
    }
    void execute()
    {
        check(cmd->Close());
        const auto next = ++value;
        check(fence->SetEventOnCompletion(next, event));
        ID3D12CommandList* lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), next));
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) {
            std::fputs("WARP fence timed out; retaining GPU resources until process exit\n", stderr);
            TerminateProcess(GetCurrentProcess(), 2);
        }
        check(device->GetDeviceRemovedReason());
    }
    void run(const char* name, const Image& previous, const Image& current, bool compare,
        uint32_t parity, float expectedSad, float expectedHistogram, bool cut, bool flash, uint32_t count)
    {
        const Image sentinel = [] { Image image{}; image.fill(0xff2964c7u); return image; }();
        auto a = texture(), b = texture(), display = texture();
        auto upload = buffer(3 * imageBytes + 4, D3D12_HEAP_TYPE_UPLOAD);
        auto readback = buffer(512 + imageBytes, D3D12_HEAP_TYPE_READBACK);
        void* mapped{};
        D3D12_RANGE empty{0, 0};
        check(upload->Map(0, &empty, &mapped));
        auto* bytes = static_cast<uint8_t*>(mapped);
        std::memcpy(bytes, previous.data(), imageBytes);
        std::memcpy(bytes + imageBytes, current.data(), imageBytes);
        std::memcpy(bytes + 2 * imageBytes, sentinel.data(), imageBytes);
        const uint32_t marker = 0x12345678u;
        std::memcpy(bytes + 3 * imageBytes, &marker, sizeof(marker));
        upload->Unmap(0, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ComPtr<ID3D12DescriptorHeap> cpu, gpu;
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cpu)));
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gpu)));
        const UINT stride = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        auto source = cpu->GetCPUDescriptorHandleForHeapStart();
        auto destination = gpu->GetCPUDescriptorHandleForHeapStart();
        for (auto* texture : {a.Get(), b.Get()}) {
            device->CreateShaderResourceView(texture, nullptr, source);
            device->CopyDescriptorsSimple(1, destination, source, heapDesc.Type);
            source.ptr += stride; destination.ptr += stride;
        }
        check(allocator->Reset());
        check(cmd->Reset(allocator.Get(), nullptr));
        ID3D12Resource* textures[]{a.Get(), b.Get(), display.Get()};
        for (UINT i = 0; i < 3; ++i) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = UINT64{i} * imageBytes;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, pitch};
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = textures[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            auto barrier = transition(textures[i], D3D12_RESOURCE_STATE_COPY_DEST,
                i < 2 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON);
            cmd->ResourceBarrier(1, &barrier);
        }
        ID3D12DescriptorHeap* heaps[]{gpu.Get()};
        cmd->SetDescriptorHeaps(1, heaps);
        auto first = gpu->GetGPUDescriptorHandleForHeapStart(), second = first;
        second.ptr += stride;
        shot.record(cmd.Get(), {first, second, width, height, parity, compare ? 1u : 0u}, readback.Get());
        D3D12_RESOURCE_BARRIER inputs[]{
            transition(a.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(b.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
        };
        cmd->ResourceBarrier(2, inputs);
        shot.replace_on_cut(cmd.Get(), display.Get(), b.Get(), parity);
        cmd->CopyBufferRegion(readback.Get(), 128, upload.Get(), 3 * imageBytes, sizeof(marker));
        auto barrier = transition(display.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = display.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 512;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, pitch};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        execute();
        D3D12_RANGE range{0, 512 + imageBytes};
        check(readback->Map(0, &range, &mapped));
        const auto* result = static_cast<const uint8_t*>(mapped);
        const auto* words = reinterpret_cast<const uint32_t*>(result + 256);
        float sad{}, histogram{};
        std::memcpy(&sad, &words[4 + 4 * parity], sizeof(sad));
        std::memcpy(&histogram, &words[5 + 4 * parity], sizeof(histogram));
        uint32_t actualMarker{};
        std::memcpy(&actualMarker, result + 128, sizeof(actualMarker));
        require(std::abs(sad - expectedSad) < 0.0001f, "SAD mismatch");
        require(std::abs(histogram - expectedHistogram) < 0.0001f, "histogram mismatch");
        require(words[parity * 2] == (cut ? 1u : 0u) && words[parity * 2 + 1] == 0, "predicate mismatch");
        require(words[6 + 4 * parity] == (compare ? 1u : 0u), "compare mismatch");
        require(words[7 + 4 * parity] == (flash ? 1u : 0u), "flash mismatch");
        require(words[12] == count, "cut counter mismatch");
        require(actualMarker == marker, "unpredicated marker did not execute");
        const auto& expected = cut ? current : sentinel;
        require(std::memcmp(result + 512, expected.data(), imageBytes) == 0, "display bytes mismatch");
        uint64_t byteHash = 14695981039346656037ull;
        for (UINT i = 0; i < imageBytes; ++i)
            byteHash = (byteHash ^ result[512 + i]) * 1099511628211ull;
        std::printf("%s sad=%.6f histogram=%.6f cut=%u flash=%u count=%u marker=%08x bytes=%u fnv64=%016llx PASS\n",
            name, sad, histogram, cut ? 1u : 0u, flash ? 1u : 0u, count, actualMarker, imageBytes,
            static_cast<unsigned long long>(byteHash));
        readback->Unmap(0, &empty);
    }
};
uint32_t gray(uint32_t x) { return 0xff000000u | x | (x << 8) | (x << 16); }
#include "vfg_shot_change_compare_test.h"
}
int main()
{
    try {
        std::printf("warp_fixture_pid=%lu\n", GetCurrentProcessId());
        Fixture fixture;
        Image black{}, white{}, panA{}, panB{}, checkerA{}, checkerB{};
        black.fill(gray(0)); white.fill(gray(255));
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            const auto i = y * width + x;
            panA[i] = gray(4 * x); panB[i] = gray(4 * ((x + 1) % width));
            checkerA[i] = gray(((x + y) & 1) ? 255 : 0);
            checkerB[i] = gray(((x + y) & 1) ? 0 : 255);
        }
        fixture.run("pan", panA, panB, true, 0, 504.0f / 16320.0f, 0, false, false, 0);
        fixture.run("cut", black, white, true, 1, 1, 1, true, false, 1);
        fixture.run("flash", checkerA, checkerB, true, 0, 1, 0, false, true, 1);
        fixture.run("cold", black, white, false, 1, 0, 0, false, false, 1);
        fixture.run("reuse-cut", black, white, true, 0, 1, 1, true, false, 2);
        fixture.run("reuse-same", black, black, true, 1, 0, 0, false, false, 2);
        compare_readback_test(fixture);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
