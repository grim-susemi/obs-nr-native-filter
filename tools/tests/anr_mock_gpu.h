// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "anr_commands.h"
#include "anr_d3d.h"
#include "anr_fg_backend.h"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <cstring>
#include <stdexcept>

namespace anr_test {
using namespace nr::fg::vfg;
using namespace nr;
constexpr Extent extent{640, 360};
constexpr uint64_t serialBase = 0x100000000ull;
constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
constexpr auto source = D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr auto destination = D3D12_RESOURCE_STATE_COPY_DEST;
using Image = std::vector<uint32_t>;
inline bool smokeWithoutDebugLayer{};
inline void require(bool passed, const char* why) { if (!passed) throw std::runtime_error(why); }

struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"ANR test fixture", WS_OVERLAPPED,
        0, 0, extent.width, extent.height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (handle) DestroyWindow(handle); }
};

struct Graphics {
    Window window;
    std::unique_ptr<Gpu> gpu;
    ComPtr<ID3D12InfoQueue> diagnostics;
    std::unique_ptr<AnrCommands> commands;
    explicit Graphics(bool warp)
    {
        require(window.handle != nullptr, "create test window");
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "fixture DXGI factory");
        if (warp) {
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "fixture WARP adapter");
        } else {
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                check(adapter->GetDesc1(&desc), "fixture adapter properties");
                if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
                adapter.Reset();
            }
        }
        require(adapter.Get() != nullptr, "fixture hardware adapter unavailable");
        check(adapter->GetDesc1(&desc), "fixture adapter description");
        std::printf("gpu_adapter=%ls\n", desc.Description);
        Session session{};
        session.backend = Backend::AnrExperimental;
        session.width = extent.width;
        session.height = extent.height;
        session.gpuShotChange = 1;
        session.adapter = desc.AdapterLuid;
        gpu = std::make_unique<Gpu>(session, window.handle, 4);
        if (!smokeWithoutDebugLayer)
            check(gpu->d3d_device()->QueryInterface(IID_PPV_ARGS(&diagnostics)), "fixture D3D12 debug layer");
        commands = std::make_unique<AnrCommands>(gpu->d3d_device(), gpu->drawQueue.Get());
    }
    void drain()
    {
        const auto done = commands->seal();
        commands->wait_for(done);
    }
    template<class Action> void record(Action action)
    {
        auto* list = commands->begin();
        action(list);
        commands->wait_for(commands->submit(list));
        commands->recycle();
    }
    ComPtr<ID3D12Resource> allocate(D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> result;
        check(gpu->d3d_device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
            type == D3D12_HEAP_TYPE_READBACK ? destination : common, nullptr, IID_PPV_ARGS(&result)), "fixture resource");
        return result;
    }
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format)
    {
        auto desc = gpu->input[0]->GetDesc();
        desc.Format = format;
        return allocate(desc, D3D12_HEAP_TYPE_DEFAULT);
    }
    Image transfer(ID3D12Resource* texture, const Image* pixels = nullptr)
    {
        const auto desc = texture->GetDesc();
        D3D12_TEXTURE_COPY_LOCATION packed{}, target{};
        packed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        UINT64 bytes{};
        gpu->d3d_device()->GetCopyableFootprints(&desc, 0, 1, 0, &packed.PlacedFootprint, nullptr, nullptr, &bytes);
        auto buffer = desc;
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
        auto staging = allocate(buffer, pixels ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK);
        packed.pResource = staging.Get();
        target.pResource = texture;
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const auto pitch = packed.PlacedFootprint.Footprint.RowPitch;
        const auto columns = static_cast<size_t>(desc.Width);
        const auto rowBytes = columns * sizeof(uint32_t);
        void* mapped{};
        const D3D12_RANGE none{0, 0}, full{0, static_cast<size_t>(bytes)};
        if (pixels) {
            require(pixels->size() == columns * desc.Height, "fixture upload extent");
            check(staging->Map(0, &none, &mapped), "fixture write map");
            for (size_t y = 0; y < desc.Height; ++y)
                std::memcpy(static_cast<std::byte*>(mapped) + y * pitch, pixels->data() + y * columns, rowBytes);
            staging->Unmap(0, &full);
        }
        record([&](ID3D12GraphicsCommandList* list) {
            const auto state = pixels ? destination : source;
            transition(list, texture, common, state);
            list->CopyTextureRegion(pixels ? &target : &packed, 0, 0, 0, pixels ? &packed : &target, nullptr);
            transition(list, texture, state, common);
        });
        if (pixels) return {};
        check(staging->Map(0, &full, &mapped), "fixture read map");
        Image result(columns * desc.Height);
        for (size_t y = 0; y < desc.Height; ++y)
            std::memcpy(result.data() + y * columns, static_cast<const std::byte*>(mapped) + y * pitch, rowBytes);
        staging->Unmap(0, &none);
        return result;
    }
    void verify_debug() const
    {
        if (smokeWithoutDebugLayer) {
            std::puts("d3d12_state_errors=UNVERIFIED (diagnostic smoke without debug layer)");
            return;
        }
        UINT64 errors{};
        for (UINT64 i = 0; i < diagnostics->GetNumStoredMessages(); ++i) {
            SIZE_T bytes{};
            check(diagnostics->GetMessage(i, nullptr, &bytes), "debug message size");
            std::vector<std::byte> storage(bytes);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            check(diagnostics->GetMessage(i, msg, &bytes), "debug message");
            if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                ++errors;
                std::fprintf(stderr, "%s\n", msg->pDescription);
            }
        }
        std::printf("d3d12_state_errors=%llu\n", static_cast<unsigned long long>(errors));
        require(errors == 0, "D3D12 debug layer reported errors");
    }
};

class FixtureGuides final : public IAnrGuideProvider {
    ComPtr<ID3D12Resource> motion_, depth_;
    std::optional<FlowResult> output_;
    uint64_t sequence_{}, epoch_{};
public:
    explicit FixtureGuides(Graphics& gpu)
        : motion_(gpu.texture(DXGI_FORMAT_R16G16_FLOAT)), depth_(gpu.texture(DXGI_FORMAT_R32_FLOAT))
    {
        const Image motion(extent.width * extent.height, 0);
        const Image depth(extent.width * extent.height, 0x3f800000u);
        gpu.transfer(motion_.Get(), &motion);
        gpu.transfer(depth_.Get(), &depth);
    }
    FlowProblem submit(const FlowInput& input) override
    {
        require(!output_ && input.current.key.frame.ingressSequence == sequence_ + 1,
            "guide single flight and dense chronology");
        require(input.reset ? !input.previous && input.current.key.historyEpoch == epoch_ + 1 :
            input.previous && input.previous->key.frame.ingressSequence == sequence_ &&
                input.current.key.historyEpoch == epoch_, "guide reset/reference chronology");
        sequence_ = input.current.key.frame.ingressSequence;
        epoch_ = input.current.key.historyEpoch;
        output_.emplace(FlowResult{input.current.key, motion_.Get(), depth_.Get(), nullptr, nullptr,
            input.current.ready, input.reset, nullptr, nullptr, nullptr, {}, {}, {}, {}, {}, {}, nullptr, nullptr, {}});
        return FlowProblem::None;
    }
    const FlowResult* result() const override { return output_ ? &*output_ : nullptr; }
    bool collect(const JobKey& key, NV_OF_FENCE_POINT fence) override
    {
        if (!output_ || output_->key != key || fence.fence->GetCompletedValue() < fence.value) return false;
        output_.reset();
        return true;
    }
    bool close() override { return !output_; }
};

inline FgPairDesc pair(Graphics& graphics, unsigned current, unsigned serial, unsigned multiplier)
{
    auto& gpu = *graphics.gpu;
    return {gpu.input[current ^ 1u].Get(), gpu.input[current].Get(),
        serialBase + serial - 1, serialBase + serial, multiplier, false};
}
} // namespace anr_test
