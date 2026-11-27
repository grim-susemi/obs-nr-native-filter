// SPDX-License-Identifier: GPL-3.0-only
// Contract + real-device lane for the Shared NVOF consumer policy.
//
// It proves the three ownership states the presenter must distinguish:
//   NrOwned + Valid          -> imported motion is consumed, local OFA never runs
//   NrOwned + Held/Unmatched -> no speculative local OFA (pair is withheld)
//   ConsumerFallback         -> local analysis runs normally
// plus the render/guide scale conversion, the 60 Hz interval fallback and the
// sanitized passthrough when the guide already matches the render extent. The
// SR colour-delay decision keys the displayed colour tick by the SR's own
// snapshot, never by an output-extent change.
#include "anr_guides.h"
#include "fg_backend.h"
#include "nr_flow_broker.h"
#include "obs_filter.h"
#include "sr_display.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace nr;
using namespace nr::fg::vfg;
namespace ipc = nr::fg::ipc;
using Microsoft::WRL::ComPtr;

namespace {
unsigned checks = 0;
void expect(bool value, const char* why)
{
    ++checks;
    if (!value) throw std::runtime_error(why);
}

struct CountingGuides final : IAnrGuideProvider {
    unsigned submits = 0, collects = 0, closes = 0;
    FlowProblem submit(const FlowInput&) override
    {
        ++submits;
        return FlowProblem::None;
    }
    const FlowResult* result() const override { return nullptr; }
    bool collect(const JobKey&, NV_OF_FENCE_POINT) override
    {
        ++collects;
        return true;
    }
    bool close() override
    {
        ++closes;
        return true;
    }
};

uint16_t half_of(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu) return static_cast<uint16_t>(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    const auto adjusted = static_cast<int32_t>(exponent) - 112;
    if (adjusted >= 0x1f) return static_cast<uint16_t>(sign | 0x7c00u);
    if (adjusted <= 0) {
        if (adjusted < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        return static_cast<uint16_t>(sign | (mantissa >> static_cast<uint32_t>(14 - adjusted)));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(adjusted) << 10) | (mantissa >> 13));
}

float float_of(uint16_t value)
{
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    const uint32_t exponent = (value >> 10) & 0x1fu;
    const uint32_t mantissa = value & 0x3ffu;
    if (!exponent) {
        if (!mantissa) {
            uint32_t bits = sign;
            float out = 0;
            std::memcpy(&out, &bits, sizeof out);
            return out;
        }
        const float fraction = static_cast<float>(mantissa) / 1024.0f;
        const float out = fraction * std::ldexp(1.0f, -14);
        return sign ? -out : out;
    }
    uint32_t bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float out = 0;
    std::memcpy(&out, &bits, sizeof out);
    return out;
}

void checked(HRESULT result, const char* why)
{
    if (FAILED(result)) throw std::runtime_error(why);
}

struct Device {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t serial = 0;
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device()
    {
        if (event) CloseHandle(event);
    }
    bool open()
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter.Reset();
        }
        if (!adapter) return false;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;
        const D3D12_COMMAND_QUEUE_DESC queueDesc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }
    void execute()
    {
        checked(list->Close(), "close shared flow list");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        checked(queue->Signal(fence.Get(), ++serial), "shared flow fence signal");
        checked(fence->SetEventOnCompletion(serial, event), "shared flow fence arm");
        expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "shared flow fence wait");
        checked(allocator->Reset(), "shared flow allocator reset");
        checked(list->Reset(allocator.Get(), nullptr), "shared flow list reset");
    }
};

ComPtr<ID3D12Resource> texture(Device& gpu, Extent extent, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = extent.width;
    desc.Height = extent.height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> out;
    checked(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
        IID_PPV_ARGS(&out)), "shared flow test texture");
    return out;
}

// Fill a guide-sized motion texture with one constant vector.
void fill_motion(Device& gpu, ID3D12Resource* motion, uint32_t width, uint32_t height, float x, float y)
{
    const uint64_t bytes = uint64_t{width} * height * 4;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    checked(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "shared flow upload buffer");
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    checked(upload->Map(0, &noRead, &mapped), "shared flow upload map");
    auto* pixels = static_cast<uint16_t*>(mapped);
    for (uint32_t i = 0; i < width * height; ++i) {
        pixels[i * 2] = half_of(x);
        pixels[i * 2 + 1] = half_of(y);
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
    from.PlacedFootprint.Footprint.Width = width;
    from.PlacedFootprint.Footprint.Height = height;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = width * 4;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = motion;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {motion, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COPY_DEST};
    gpu.list->ResourceBarrier(1, &barrier);
    gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    gpu.list->ResourceBarrier(1, &barrier);
    gpu.execute();
}

float read_motion_x(Device& gpu, ID3D12Resource* motion)
{
    D3D12_RESOURCE_DESC desc = motion->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    checked(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "shared flow readback buffer");
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {motion, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COPY_SOURCE};
    gpu.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = motion;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = layout;
    gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    gpu.list->ResourceBarrier(1, &barrier);
    gpu.execute();
    void* mapped = nullptr;
    const D3D12_RANGE read{0, static_cast<SIZE_T>(total)};
    checked(readback->Map(0, &read, &mapped), "shared flow readback map");
    const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped) + layout.Offset);
    const float value = float_of(row[0]);
    const D3D12_RANGE noWrite{0, 0};
    readback->Unmap(0, &noWrite);
    return value;
}

JobKey key_of(uint64_t serial)
{
    return JobKey{{1, 1, serial}, 1, 1, serial};
}

void routing_contract()
{
    expect(guide_interval_ns(0) == 16'666'666, "60 Hz fallback interval");
    expect(guide_interval_ns(8'333'333) == 8'333'333, "transport interval preserved");
    ImportedFlow flow;
    expect(flow_routing(flow) == FlowRouting::LocalFallback, "legacy empty sample allows local");
    flow.owner = ipc::FlowOwner::NrOwned;
    flow.provenance = ipc::FlowProvenance::Valid;
    flow.motion = reinterpret_cast<ID3D12Resource*>(1);
    flow.guideWidth = 64;
    flow.guideHeight = 36;
    expect(flow_routing(flow) == FlowRouting::Shared, "NrOwned+Valid shares motion");
    flow.provenance = ipc::FlowProvenance::HeldPrevious;
    expect(flow_routing(flow) == FlowRouting::NrUnavailable, "NrOwned+held forbids local OFA");
    flow.provenance = ipc::FlowProvenance::Valid;
    flow.motion = nullptr;
    expect(flow_routing(flow) == FlowRouting::NrUnavailable, "NrOwned without motion forbids local OFA");
    flow.owner = ipc::FlowOwner::ConsumerFallback;
    flow.provenance = ipc::FlowProvenance::Unknown;
    expect(flow_routing(flow) == FlowRouting::LocalFallback, "consumer fallback runs local OFA");
    flow.owner = ipc::FlowOwner::Unavailable;
    flow.provenance = ipc::FlowProvenance::Reset;
    expect(flow_routing(flow) == FlowRouting::NrUnavailable, "explicit reset provenance stays NR-owned");
}

// Follow-up to Todo 5: the SR colour delay must come from the SR's own per-draw
// snapshot, never from an output-extent change. The extent proxy mis-keys the
// shared pair in both directions: DLSS at 1:1 delays without changing extent,
// and bilinear 1080p->1440p changes extent without delaying.
void sr_delay_contract()
{
    constexpr std::uint64_t interval = 8'333'333; // 120 Hz
    const std::uint64_t capture = 1200 * interval + interval / 2;
    const auto canonical = sr_canonical_tick(capture, interval);
    expect(canonical == nr::sampler::canonical_tick(capture, interval),
        "FG display tick matches the OBS canonical token");
    const auto previous = capture - interval;

    // (a) DLSS running natively 1:1 presents the previous capture. The output
    // extent never changes, so only the snapshot can report the delay.
    const SrPresentSignal dlss{true, true, false, previous, capture};
    const auto a = sr_display_decision({capture, interval, false}, dlss);
    const FlowColorKey dlssDisplayed{7, canonical - 1, 3, 11, 42, 5};
    const FramePairKey dlssPair{{7, canonical - 3, 3, 11, 40, 5}, {7, canonical - 2, 3, 11, 41, 5}};
    expect(dlssPair.valid(), "DLSS fixture pair is a valid adjacent pair");
    expect(a.path == SrDelayPath::Snapshot && a.shift == 1, "DLSS 1:1 delays through the snapshot");
    expect(a.displayTick == dlssDisplayed.canonicalTick, "DLSS 1:1 keys the displayed colour tick");
    expect(FlowBroker::display_tick_for(dlssPair, a.displayTick), "DLSS 1:1 pair serves the displayed colour");
    expect(!FlowBroker::display_tick_for(dlssPair, canonical), "the unshifted tick keys another pair");
    std::puts("PASS anr_shared_flow case=dlss-native-1to1-delays-without-extent-change");

    // (b) Bilinear 1080p->1440p changes extent but shows this tick's colour.
    const SrPresentSignal bilinear{true, false, true, previous, previous};
    const auto b = sr_display_decision({capture, interval, true}, bilinear);
    const FlowColorKey bilinearDisplayed{7, canonical, 3, 11, 44, 5};
    const FramePairKey bilinearPair{{7, canonical - 2, 3, 11, 42, 5}, {7, canonical - 1, 3, 11, 43, 5}};
    expect(bilinearPair.valid(), "bilinear fixture pair is a valid adjacent pair");
    expect(b.path == SrDelayPath::Snapshot && b.shift == 0, "bilinear extent change never delays");
    expect(b.displayTick == bilinearDisplayed.canonicalTick, "bilinear keys the displayed colour tick");
    expect(FlowBroker::display_tick_for(bilinearPair, b.displayTick), "bilinear pair serves the displayed colour");
    expect(b.displayTick != canonical - 1, "the extent proxy would have shifted the bilinear colour");
    std::puts("PASS anr_shared_flow case=bilinear-1080p-to-1440p-extent-change-without-delay");

    // (c) No SR telemetry at all: the documented fallback is reported (the tap
    // logs it once per generation) and keeps the one-tick shift.
    const auto c = sr_display_decision({capture, interval, true}, SrPresentSignal{});
    expect(c.path == SrDelayPath::ExtentFallback, "absent snapshot reports the documented fallback");
    expect(c.shift == 1 && c.displayTick == canonical - 1, "fallback keeps the one-tick shift");
    const auto noChange = sr_display_decision({capture, interval, false}, SrPresentSignal{});
    expect(noChange.path == SrDelayPath::ExtentFallback && noChange.shift == 0,
        "fallback without an extent change does not delay");
    std::puts("PASS anr_shared_flow case=snapshot-absent-documented-extent-fallback");

    // Telemetry that did not advance for this capture (DLSS stopped mid-session)
    // must never delay a spatial frame.
    const SrPresentSignal stale{true, true, false, previous, previous};
    const auto s = sr_display_decision({capture, interval, true}, stale);
    expect(s.path == SrDelayPath::Snapshot && s.shift == 0, "stale temporal telemetry never delays");
}

void shared_flow_case(Device& gpu)
{
    const Extent guide{64, 36};
    const Extent render{96, 54};
    auto imported = texture(gpu, guide, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COMMON);
    fill_motion(gpu, imported.Get(), guide.width, guide.height, 8.0f, 0.0f);

    auto counter = std::make_unique<CountingGuides>();
    auto* local = counter.get();
    auto shared = make_shared_anr_guides(gpu.device.Get(), render, default_frame_interval_ns, std::move(counter));

    ImportedFlow flow{};
    flow.motion = imported.Get();
    flow.guideWidth = guide.width;
    flow.guideHeight = guide.height;
    flow.mvScaleX = static_cast<float>(render.width) / guide.width;
    flow.mvScaleY = static_cast<float>(render.height) / guide.height;
    flow.owner = ipc::FlowOwner::NrOwned;
    flow.provenance = ipc::FlowProvenance::Valid;
    flow.ready = {};
    shared->configure(flow);
    FlowProblem problem = FlowProblem::InvalidKey;
    const FlowImage image{key_of(1), nullptr, NV_OF_FENCE_POINT{}};
    expect(shared->shared_path(gpu.list.Get(), image, false, problem), "valid pair takes the shared path");
    expect(problem == FlowProblem::None, "shared path reports no problem");
    const auto* result = shared->result();
    expect(result && result->key == image.key, "shared result carries the pair key");
    expect(result->motion != imported.Get(), "different extent is rescaled");
    const auto stats = shared->stats();
    expect(stats.ofaExecutions == 0 && stats.sharedHits == 1, "shared hit without local OFA");
    expect(local->submits == 0, "local provider never submitted");
    gpu.execute();
    const float scaled = read_motion_x(gpu, result->motion);
    expect(std::fabs(scaled - 12.0f) <= 0.05f, "8 guide px * 1.5 = 12 render px");

    // Equal extents pass the imported resource through untouched.
    shared->configure(ImportedFlow{imported.Get(), render.width, render.height, 1.0f, 1.0f,
        ipc::FlowOwner::NrOwned, ipc::FlowProvenance::Valid, {}, false, false});
    problem = FlowProblem::InvalidKey;
    expect(shared->shared_path(gpu.list.Get(), image, false, problem), "equal extent takes the shared path");
    expect(shared->result() && shared->result()->motion == imported.Get(), "equal extent passes motion directly");
    gpu.execute();

    // A reset pair publishes zero motion instead of any stale imported vector.
    shared->configure(ImportedFlow{imported.Get(), render.width, render.height, 1.0f, 1.0f,
        ipc::FlowOwner::NrOwned, ipc::FlowProvenance::Valid, {}, false, false});
    problem = FlowProblem::InvalidKey;
    expect(shared->shared_path(gpu.list.Get(), image, true, problem), "reset pair takes the shared path");
    expect(shared->result() && shared->result()->motion != imported.Get(), "reset pair clears motion");
    expect(shared->stats().sharedResets == 1 && shared->stats().sharedHits == 2, "reset frame is not a shared hit");
    gpu.execute();

    // NR-owned without a usable pair: the backend withholds the pair, and the
    // provider must not silently run local analysis.
    ImportedFlow held = flow;
    held.provenance = ipc::FlowProvenance::HeldPrevious;
    shared->configure(held);
    problem = FlowProblem::InvalidKey;
    expect(!shared->shared_path(gpu.list.Get(), image, false, problem), "held pair is not a shared path");
    expect(local->submits == 0, "held pair never runs local OFA");
    expect(shared->stats().sharedUnavailable == 1, "held pair is counted unavailable");

    // Consumer fallback: NR is absent/disabled, so local guides run.
    shared->configure(ImportedFlow{nullptr, 0, 0, 1.0f, 1.0f, ipc::FlowOwner::ConsumerFallback,
        ipc::FlowProvenance::Unknown, {}, false, false});
    const auto localResult = shared->submit(FlowInput{FlowImage{key_of(2), imported.Get(), NV_OF_FENCE_POINT{}},
        nullptr, guide, {0, 0, guide.width, guide.height}, false});
    expect(localResult == FlowProblem::None && local->submits == 1, "consumer fallback runs local OFA");
    expect(shared->stats().ofaExecutions == 1, "fallback increments fg_ofa_exec_count");

    expect(shared->close() && local->closes == 1, "local provider closed");
}
} // namespace

int main()
{
    try {
        routing_contract();
        sr_delay_contract();
        Device gpu;
        if (!gpu.open()) {
            std::puts("SKIP anr_shared_flow: no hardware D3D12 adapter");
            return 0;
        }
        shared_flow_case(gpu);
        std::printf("PASS anr_shared_flow checks=%u\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL anr_shared_flow: %s\n", error.what());
        return 1;
    }
}
