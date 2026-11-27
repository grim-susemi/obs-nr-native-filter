// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - the todo admits one new test source, so the D3D12 fixture,
// its frame cases and the CPU pinned-slot checks share this file.
// D3D12 fixture for the band-tolerant content comparison kernel plus the
// pinned-slot ContentHistory lookup. WARP is the default adapter; hardware
// D3D12 is used only when WARP is unavailable, and the choice is printed.
#include "vfg_content.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include "vfg_content_compare_band_shader.h"

using Microsoft::WRL::ComPtr;
using nr::fg::vfg::ContentHistory;
using nr::fg::vfg::content_input_count;
using nr::fg::vfg::content_over_budget;

namespace {
constexpr UINT width = 128, height = 64, pitch = width * 4, imageBytes = pitch * height;
constexpr UINT changedBytes = 256, changedUints = changedBytes / sizeof(uint32_t);
constexpr uint32_t band = 3;
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

uint32_t rgba(UINT r, UINT g, UINT b) { return r | (g << 8) | (b << 16) | 0xff000000u; }
uint32_t shift(uint32_t pixel, int r, int g, int b)
{
    return rgba(static_cast<UINT>(static_cast<int>(pixel & 0xffu) + r),
        static_cast<UINT>(static_cast<int>((pixel >> 8) & 0xffu) + g),
        static_cast<UINT>(static_cast<int>((pixel >> 16) & 0xffu) + b));
}

struct Case {
    const char* name;
    const Image& previous;
    const Image& current;
    uint32_t mode;
    uint32_t over;
    uint32_t maxDiff;
    bool same;
};

struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    HANDLE event_{};
    UINT64 value_{};
    bool warp{};
    std::wstring adapter;

    Fixture()
    {
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> hardware;
        warp = SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&hardware)));
        if (!warp) check(factory->EnumAdapters1(0, &hardware));
        DXGI_ADAPTER_DESC desc{};
        check(hardware->GetDesc(&desc));
        adapter = desc.Description;
        check(D3D12CreateDevice(hardware.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&cmd)));
        check(cmd->Close());
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event_ != nullptr, "fence event");
        create_pipeline();
    }
    ~Fixture() { if (event_) CloseHandle(event_); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void create_pipeline()
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        D3D12_ROOT_PARAMETER params[4]{};
        for (UINT i = 0; i < 2; ++i) {
            ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE};
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable = {1, &ranges[i]};
            params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor = {0, 0};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants = {0, 0, 4};
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 4;
        signature.pParameters = params;
        ComPtr<ID3DBlob> blob;
        check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr));
        check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
        state.pRootSignature = root.Get();
        state.CS = {vfg_content_compare_band_shader, sizeof(vfg_content_compare_band_shader)};
        check(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipeline)));
    }

    ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type, bool unordered = false)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (unordered) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto initial = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
            : D3D12_RESOURCE_STATE_COPY_DEST;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
            IID_PPV_ARGS(&result)));
        return result;
    }

    ComPtr<ID3D12Resource> texture()
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)));
        return result;
    }

    void execute()
    {
        check(cmd->Close());
        const auto next = ++value_;
        check(fence->SetEventOnCompletion(next, event_));
        ID3D12CommandList* lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), next));
        require(WaitForSingleObject(event_, 30000) == WAIT_OBJECT_0, "GPU fence timeout");
        check(device->GetDeviceRemovedReason());
    }

    void run(const Case& c)
    {
        auto previous = texture(), current = texture();
        auto upload = buffer(2 * imageBytes, D3D12_HEAP_TYPE_UPLOAD);
        auto changed = buffer(changedBytes, D3D12_HEAP_TYPE_DEFAULT, true);
        auto zero = buffer(changedBytes, D3D12_HEAP_TYPE_UPLOAD);
        auto readback = buffer(changedBytes, D3D12_HEAP_TYPE_READBACK);
        void* mapped{};
        const D3D12_RANGE noRead{0, 0}, written{0, changedBytes};
        check(upload->Map(0, &noRead, &mapped));
        std::memcpy(mapped, c.previous.data(), imageBytes);
        std::memcpy(static_cast<uint8_t*>(mapped) + imageBytes, c.current.data(), imageBytes);
        upload->Unmap(0, nullptr);
        check(zero->Map(0, &noRead, &mapped));
        std::memset(mapped, 0, changedBytes);
        zero->Unmap(0, &written);

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ComPtr<ID3D12DescriptorHeap> cpu, gpu;
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cpu)));
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gpu)));
        const UINT stride = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        auto source = cpu->GetCPUDescriptorHandleForHeapStart();
        auto destination = gpu->GetCPUDescriptorHandleForHeapStart();
        for (auto* target : {previous.Get(), current.Get()}) {
            device->CreateShaderResourceView(target, nullptr, source);
            device->CopyDescriptorsSimple(1, destination, source, heapDesc.Type);
            source.ptr += stride;
            destination.ptr += stride;
        }

        check(allocator->Reset());
        check(cmd->Reset(allocator.Get(), nullptr));
        ID3D12Resource* textures[]{previous.Get(), current.Get()};
        for (UINT i = 0; i < 2; ++i) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = UINT64{i} * imageBytes;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, pitch};
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = textures[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            auto barrier = transition(textures[i], D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &barrier);
        }
        cmd->CopyBufferRegion(changed.Get(), 0, zero.Get(), 0, changedBytes);
        auto toUav = transition(changed.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &toUav);

        ID3D12DescriptorHeap* heaps[]{gpu.Get()};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetPipelineState(pipeline.Get());
        auto view = gpu->GetGPUDescriptorHandleForHeapStart();
        cmd->SetComputeRootDescriptorTable(0, view);
        view.ptr += stride;
        cmd->SetComputeRootDescriptorTable(1, view);
        cmd->SetComputeRootUnorderedAccessView(2, changed->GetGPUVirtualAddress());
        const uint32_t params[4]{c.mode, band, content_over_budget(width, height), 0};
        cmd->SetComputeRoot32BitConstants(3, 4, params, 0);
        cmd->Dispatch((width + 63) / 64, (height + 15) / 16, 1);

        auto toCopy = transition(changed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &toCopy);
        cmd->CopyBufferRegion(readback.Get(), 0, changed.Get(), 0, changedBytes);
        execute();

        std::array<uint32_t, changedUints> words{};
        const D3D12_RANGE readRange{0, changedBytes};
        check(readback->Map(0, &readRange, &mapped));
        std::memcpy(words.data(), mapped, changedBytes);
        readback->Unmap(0, &noRead);

        const auto budget = content_over_budget(width, height);
        const auto nonzero = std::count_if(words.begin(), words.end(), [](uint32_t word) { return word != 0; });
        if (c.mode == 0) {
            const bool same = nonzero == 0;
            require(same == c.same, "exact-mode classification");
            std::printf("PASS %s mode=0 differing_words=%d same=%u\n", c.name, static_cast<int>(nonzero),
                same ? 1u : 0u);
            return;
        }
        const auto over = words[0], maxDiff = words[1];
        const bool same = over <= budget;
        require(std::all_of(words.begin() + 2, words.end(), [](uint32_t word) { return word == 0; }),
            "band mode writes only overCount and maxDiff");
        require(over == c.over, "over-band component count");
        require(maxDiff == c.maxDiff, "maximum component difference");
        require(same == c.same, "band-mode classification");
        std::printf("PASS %s mode=1 band=%u over=%u maxDiff=%u budget=%u same=%u\n", c.name, band, over,
            maxDiff, budget, same ? 1u : 0u);
    }
};

nr::fg::ipc::Frame frame(uint64_t serial, uint64_t timestamp)
{
    nr::fg::ipc::Frame result{};
    result.generation = 7;
    result.serial = serial;
    result.timestampNs = timestamp;
    result.arrivalNs = timestamp;
    result.shape = {1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    return result;
}

void pinned_selection()
{
    require(content_over_budget(128, 64) == 1, "small-frame budget floor");
    require(content_over_budget(1920, 1080) == 31, "five per million at 1080p");
    ContentHistory history;
    const std::array<unsigned, 1> pin0{0}, pin2{2};
    const std::array<unsigned, 2> pin01{0, 1};
    const std::array<unsigned, 3> pin012{0, 1, 2};
    const std::array<unsigned, 4> pin0123{0, 1, 2, 3};
    require(history.input_index() == 0, "fresh history starts at slot zero");
    require(history.input_index(pin0) == 1, "pinned slot zero is skipped");
    require(history.input_index(pin01) == 2, "two pinned slots leave the next slot");
    require(history.input_index(pin012) == 3, "three pinned slots leave the fourth");
    require(history.input_index(pin0123) == content_input_count, "all pinned slots are exhausted");

    const auto first = frame(1, 1'000'000'000);
    const auto second = frame(2, first.timestampNs + 16'666'667);
    const unsigned anchorSlot = history.input_index();
    require(!history.observe(anchorSlot, first, false).stale, "first frame is admitted");
    history.retain(anchorSlot, first);
    require(history.latest_admitted() && history.latest_admitted()->frame.serial == 1 &&
        history.latest_admitted()->index == anchorSlot, "latest admitted starts at the first frame");
    const unsigned transportSlot = history.input_index();
    require(transportSlot == 1 && history.observe(transportSlot, second, true).continuous,
        "adjacent second frame continues on the next slot");
    require(history.latest_admitted()->index == transportSlot && history.latest_admitted()->frame.serial == 2,
        "latest admitted advances with the transport");
    require(history.anchor()->frame.serial == 1, "presented anchor waits for retain");
    require(history.input_index() == 2, "two live slots leave two free destinations");
    require(history.input_index(pin0) == 2, "pinning an already-reserved slot changes nothing");
    require(history.input_index(pin012) == 3, "three reservations leave the fourth slot");
    require(history.input_index(pin0123) == content_input_count, "four reservations exhaust the ring");

    require(history.observe(2, first, true).stale, "rewound serial is stale");
    require(history.latest_admitted()->frame.serial == 2 && history.latest_admitted()->index == 1,
        "a stale copy cannot rewind the admitted transport");
    require(history.input_index(pin0123) == content_input_count, "a stale copy releases no pinned slot");

    history.invalidate();
    require(!history.can_compare(), "invalidate drops the pair anchor");
    require(history.latest_admitted() && history.latest_admitted()->index == 1,
        "the admitted transport survives invalidation");
    require(history.input_index() == 0, "dropping the anchor frees its slot");
    require(history.input_index(pin2) == 0, "the pin still reserves its own slot");
    std::printf("PASS pinned_index anchor=0 transport=1 free=2 latest=2 exhausted=%u stale_serial=1\n",
        content_input_count);
}
} // namespace

int main()
{
    try {
        Fixture fixture;
        std::printf("fixture adapter=%ls warp=%u budget_128x64=%u budget_1920x1080=%u\n",
            fixture.adapter.c_str(), fixture.warp ? 1u : 0u, content_over_budget(width, height),
            content_over_budget(1920, 1080));
        Image base{};
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
                base[y * width + x] = rgba(32 + (x * 2) % 160, 32 + (y * 4) % 160,
                    32 + ((x * 3 + y * 5) % 160));
        Image noise = base, atBand = base, aboveBand = base, oneOver = base, twoOver = base, block = base,
              full = base;
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const auto i = y * width + x;
                noise[i] = shift(base[i], ((x + y) & 1) ? 2 : -2, ((x ^ y) & 1) ? 2 : -2,
                    (x & 1) ? 2 : -2);
                full[i] = shift(base[i], 4, 4, 4);
            }
        }
        atBand[5 * width + 5] = shift(base[5 * width + 5], 3, 3, 3);
        aboveBand[5 * width + 5] = shift(base[5 * width + 5], 4, 4, 4);
        oneOver[5 * width + 5] = shift(base[5 * width + 5], 16, 0, 0);
        twoOver[5 * width + 5] = shift(base[5 * width + 5], 16, 16, 0);
        for (UINT y = 8; y < 24; ++y)
            for (UINT x = 56; x < 72; ++x)
                block[y * width + x] = shift(base[y * width + x], 16, 16, 16);

        fixture.run({"identical-band", base, base, 1, 0, 0, true});
        fixture.run({"identical-exact", base, base, 0, 0, 0, true});
        fixture.run({"lsb2-band", base, noise, 1, 0, 2, true});
        fixture.run({"lsb2-exact", base, noise, 0, 0, 0, false});
        fixture.run({"at-band-3", base, atBand, 1, 0, 3, true});
        fixture.run({"over-band-4", base, aboveBand, 1, 3, 4, false});
        fixture.run({"budget-one", base, oneOver, 1, 1, 16, true});
        fixture.run({"budget-two", base, twoOver, 1, 2, 16, false});
        fixture.run({"block-16x16", base, block, 1, 768, 16, false});
        fixture.run({"full-frame-4", base, full, 1, width * height * 3, 4, false});
        fixture.run({"block-16x16-exact", base, block, 0, 0, 0, false});
        fixture.run({"full-frame-4-exact", base, full, 0, 0, 0, false});
        pinned_selection();
        std::puts("PASS vfg_content_dedup cases=12");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL vfg_content_dedup: %s\n", error.what());
        return 1;
    }
}
