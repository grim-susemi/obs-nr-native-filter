// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - one deterministic QA executable owns the +8 px/frame moving-bar
// clip, the real Shared NVOF scale/sanitize lane for both extents, the SR
// single-owner counters and the interpolated-midpoint verdict.
//
// The clip moves a bar right by +8 px/frame. NR exports motion in analysis
// pixels as current -> previous, so an honest export is -8 px (negative) and the
// interpolated midpoint sits +4 px (native 1440p) or +5.33 px (= +8 * 4/3 * 0.5,
// 1080p -> 1440p) right of the previous frame's bar. The oracle rejects a
// sign-inverted export (-4.0 / -5.33), an unscaled 1440p export (+4.0 on the
// scaled arm) and a zeroed export (two half-occupied ghosts, no midpoint).
#include "anr_d3d.h"
#include "anr_guides.h"
#include "fg_backend.h"
#include "nr_flow_broker.h"
#include "obs_upscale_dlss.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
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

constexpr float kStepPx = 8.0f;      // clip motion, source pixels per frame
constexpr int kFrames = 24;          // pairs exercised by each clip lane
constexpr float kMidTolerance = 1.0f;

struct CountingGuides final : IAnrGuideProvider {
    unsigned submits = 0, closes = 0;
    FlowProblem submit(const FlowInput&) override
    {
        ++submits;
        return FlowProblem::None;
    }
    const FlowResult* result() const override { return nullptr; }
    bool collect(const JobKey&, NV_OF_FENCE_POINT) override { return true; }
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
        const float out = (static_cast<float>(mantissa) / 1024.0f) * std::ldexp(1.0f, -14);
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
        checked(list->Close(), "mv oracle list close");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        checked(queue->Signal(fence.Get(), ++serial), "mv oracle fence signal");
        checked(fence->SetEventOnCompletion(serial, event), "mv oracle fence arm");
        expect(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "mv oracle fence wait");
        checked(allocator->Reset(), "mv oracle allocator reset");
        checked(list->Reset(allocator.Get(), nullptr), "mv oracle list reset");
    }
};

ComPtr<ID3D12Resource> texture(Device& gpu, Extent extent, DXGI_FORMAT format, D3D12_RESOURCE_STATES state)
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
    ComPtr<ID3D12Resource> out;
    checked(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
        IID_PPV_ARGS(&out)), "mv oracle texture");
    return out;
}

// NR's export: R16G16_FLOAT current -> previous analysis pixels. The bar moves
// right, so the honest vector is negative. NaN marks the inactive area, exactly
// like nr_guides.hlsl outside activeRect.
void fill_motion(Device& gpu, ID3D12Resource* motion, Extent extent, float x, float y, bool nanBorder)
{
    const uint64_t bytes = static_cast<uint64_t>(extent.width) * extent.height * 4;
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
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "mv oracle upload buffer");
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    checked(upload->Map(0, &noRead, &mapped), "mv oracle upload map");
    auto* pixels = static_cast<uint16_t*>(mapped);
    const uint16_t nan = half_of(std::nanf(""));
    for (uint32_t row = 0; row < extent.height; ++row) {
        for (uint32_t col = 0; col < extent.width; ++col) {
            const bool outside = nanBorder && (row >= extent.height - 2 || col >= extent.width - 2);
            pixels[(static_cast<size_t>(row) * extent.width + col) * 2] = outside ? nan : half_of(x);
            pixels[(static_cast<size_t>(row) * extent.width + col) * 2 + 1] = outside ? nan : half_of(y);
        }
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
    from.PlacedFootprint.Footprint.Width = extent.width;
    from.PlacedFootprint.Footprint.Height = extent.height;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = extent.width * 4;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = motion;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    transition(gpu.list.Get(), motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(gpu.list.Get(), motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    gpu.execute();
}

struct MotionSample {
    float x = 0, y = 0;
    size_t nanTexels = 0;
    size_t texels = 0;
};

MotionSample read_motion(Device& gpu, ID3D12Resource* motion)
{
    const auto desc = motion->GetDesc();
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
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "mv oracle readback buffer");
    transition(gpu.list.Get(), motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = motion;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = readback.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = layout;
    gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(gpu.list.Get(), motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    gpu.execute();
    void* mapped = nullptr;
    const D3D12_RANGE read{0, static_cast<SIZE_T>(total)};
    checked(readback->Map(0, &read, &mapped), "mv oracle readback map");
    MotionSample sample;
    sample.texels = static_cast<size_t>(desc.Width) * desc.Height;
    const auto* rows = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped) + layout.Offset);
    for (uint32_t row = 0; row < desc.Height; ++row) {
        const auto* line = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(rows) + static_cast<size_t>(row) * layout.Footprint.RowPitch);
        for (uint32_t col = 0; col < desc.Width; ++col) {
            const uint16_t hx = line[col * 2], hy = line[col * 2 + 1];
            if ((hx & 0x7c00u) == 0x7c00u && (hx & 0x3ffu)) ++sample.nanTexels;
            if (row == desc.Height / 2 && col == desc.Width / 2) {
                sample.x = float_of(hx);
                sample.y = float_of(hy);
            }
        }
    }
    const D3D12_RANGE noWrite{0, 0};
    readback->Unmap(0, &noWrite);
    return sample;
}

JobKey key_of(uint64_t serial)
{
    return JobKey{{1, 1, serial}, 1, 1, serial};
}

// Analytic 2D interpolation over the measured displacement: a pixel at d reads
// prev at d + t*D and curr at d - (1-t)*D. With an honest current->previous
// export the bar lands halfway between its two positions.
struct Midpoint {
    double centroid = 0;
    double left = 0;
    double width = 0;
    double peak = 0;
    bool ok = false;
};

Midpoint measure_midpoint(double xPrev, double stepOut, int barWidth, double mv, double t = 0.5)
{
    const double xCurr = xPrev + stepOut;
    const int lo = static_cast<int>(std::floor(xPrev)) - 64;
    const int hi = static_cast<int>(std::ceil(xCurr)) + barWidth + 64;
    std::vector<double> row(static_cast<size_t>(hi - lo), 0.0);
    const auto inside = [](double p, double start, int width) { return p >= start && p < start + width; };
    for (int d = lo; d < hi; ++d) {
        const double fromPrev = 0.5 * (inside(d + t * mv, xPrev, barWidth) ? 1.0 : 0.0);
        const double fromCurr = 0.5 * (inside(d - (1.0 - t) * mv, xCurr, barWidth) ? 1.0 : 0.0);
        row[static_cast<size_t>(d - lo)] = fromPrev + fromCurr;
    }
    Midpoint out;
    double mass = 0, sum = 0;
    int first = -1, last = -1;
    for (size_t i = 0; i < row.size(); ++i) {
        out.peak = std::max(out.peak, row[i]);
        if (row[i] >= 0.99) {
            if (first < 0) first = static_cast<int>(i);
            last = static_cast<int>(i);
            mass += row[i];
            sum += (lo + static_cast<double>(i) + 0.5) * row[i];
        }
    }
    if (mass <= 0 || out.peak < 0.99) return out;
    out.centroid = sum / mass;
    out.left = lo + first + 0.5;
    out.width = static_cast<double>(last - first + 1);
    out.ok = true;
    return out;
}

// A correct interpolated bar keeps the clip bar's width and sits at the
// midpoint. A sign-inverted, zeroed or unscaled export leaves a second ghost
// next to the expected band, so the fully occupied band widens (or its centroid
// leaves the tolerance); both are rejected.
bool accepts_midpoint(const Midpoint& m, double expectedCentroid, int barWidth)
{
    return m.ok && std::fabs(m.width - barWidth) <= 2.0 &&
        std::fabs(m.centroid - expectedCentroid) <= kMidTolerance;
}

struct Arm {
    const char* name;
    Extent guide;   // NR analysis extent
    Extent render;  // SR/FG display extent
    double stepOut; // clip step in output px (+8 or +8*4/3)
    double mvOut;   // honest current->previous output px (-8 or -10.67)
    double expectMidDelta; // midpoint offset from the previous bar, +4.0 or +5.33
};

void run_arm(Device& gpu, const Arm& arm, bool invert)
{
    const bool scaled = arm.guide.width != arm.render.width;
    const auto motion = texture(gpu, arm.guide, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON);
    const float mvGuide = static_cast<float>(-kStepPx * (invert ? -1.0f : 1.0f));
    // NR sanitizes the exported slot before publishing it, so the clip lane
    // carries no NaN; the dedicated sanitize lane below proves the consumer
    // removes an unclean border instead of leaking it into interpolation.
    fill_motion(gpu, motion.Get(), arm.guide, mvGuide, 0.0f, false);

    auto counting = std::make_unique<CountingGuides>();
    auto* local = counting.get();
    auto shared = make_shared_anr_guides(gpu.device.Get(), arm.render, default_frame_interval_ns, std::move(counting));
    ImportedFlow flow{};
    flow.motion = motion.Get();
    flow.guideWidth = arm.guide.width;
    flow.guideHeight = arm.guide.height;
    flow.mvScaleX = static_cast<float>(arm.render.width) / static_cast<float>(arm.guide.width);
    flow.mvScaleY = static_cast<float>(arm.render.height) / static_cast<float>(arm.guide.height);
    flow.owner = ipc::FlowOwner::NrOwned;
    flow.provenance = ipc::FlowProvenance::Valid;
    flow.ready = {};

    size_t sharedHits = 0;
    for (int frame = 0; frame < kFrames; ++frame) {
        shared->configure(flow);
        FlowProblem problem = FlowProblem::InvalidKey;
        const FlowImage image{key_of(static_cast<uint64_t>(frame + 1)), nullptr, NV_OF_FENCE_POINT{}};
        expect(shared->shared_path(gpu.list.Get(), image, false, problem), "NrOwned+Valid must take the shared path");
        expect(problem == FlowProblem::None, "shared MV path reported a problem");
        if (shared->stats().sharedHits > sharedHits) ++sharedHits;
        gpu.execute();
    }
    const auto stats = shared->stats();
    const auto pairs = stats.sharedHits + stats.sharedUnavailable;
    const double hitRate = pairs ? static_cast<double>(stats.sharedHits) / static_cast<double>(pairs) : 0.0;
    expect(stats.sharedHits == kFrames && sharedHits == static_cast<size_t>(kFrames), "shared hits below the clip frame count");
    expect(stats.ofaExecutions == 0 && local->submits == 0, "fg_ofa_exec_count must be 0 while NR owns the pair");
    expect(hitRate >= 0.95, "shared_mv_hit_rate below 0.95");

    // SR sees the same clip through its own production scale/counter contract.
    SrFlowInput sr{};
    sr.nrPresent = sr.nrEnabled = sr.nrReady = sr.displayFound = sr.pairAvailable = true;
    sr.owner = FlowOwnerPolicy::NrOwned;
    sr.provenance = nr::FlowProvenance::Valid;
    SrFlowCounters counters;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto mode = sr_flow_mode(sr);
        expect(mode == SrFlowMode::UseShared, "NR-owned 1080p/1440p pair must use the shared MV");
        counters.observe(mode, false);
    }
    expect(counters.ofaExec == 0 && counters.sharedHits == static_cast<unsigned>(kFrames),
        "sr_ofa_exec_count must be 0 while NR owns the pair");
    // SR reads the broker's unit scale and applies guide->output itself; the FG
    // consumer reads the ratio fg_tap publishes. Both must land on one scale.
    const auto srScale = sr_motion_scale(arm.guide.width, arm.guide.height, 1.0f, 1.0f, arm.guide.width,
        arm.guide.height, arm.render.width, arm.render.height);
    expect(std::fabs(srScale.x - flow.mvScaleX) < 1e-5f, "SR and FG disagree on the guide->output scale");

    // Measured consumer export after the real scale/sanitize pass.
    const auto* consumer = shared->result();
    expect(consumer && consumer->motion, "shared result has no motion");
    const auto sample = read_motion(gpu, consumer->motion);
    expect(sample.texels > 0 && sample.nanTexels == 0, "consumer MV texture still contains NaNs");
    const auto expectMv = static_cast<float>(arm.mvOut);
    if (std::fabs(sample.x - expectMv) > 0.02f)
        throw std::runtime_error(std::string("midpoint mismatch: shared MV x=") + std::to_string(sample.x) +
            " expected " + std::to_string(expectMv) + " (" + arm.name + ")");

    // The bar is 120 px wide in analysis pixels; it scales with the guide.
    const int barOut = static_cast<int>(std::lround(120.0 * arm.render.width / arm.guide.width));
    const double xPrevOut = 320.0 * arm.render.width / arm.guide.width;
    const auto honest = measure_midpoint(xPrevOut, arm.stepOut, barOut, sample.x);
    const double expectedCentroid = xPrevOut + barOut / 2.0 + arm.expectMidDelta;
    if (!accepts_midpoint(honest, expectedCentroid, barOut))
        throw std::runtime_error(std::string("midpoint mismatch: centroid=") + std::to_string(honest.centroid) +
            " width=" + std::to_string(honest.width) + " expected " + std::to_string(expectedCentroid) + " (" +
            arm.name + ")");

    // Negative controls: the same evaluator must reject every wrong export.
    expect(!accepts_midpoint(measure_midpoint(xPrevOut, arm.stepOut, barOut, -sample.x), expectedCentroid, barOut),
        "sign-inverted MV must not pass the midpoint verdict");
    expect(!accepts_midpoint(measure_midpoint(xPrevOut, arm.stepOut, barOut, 0.0), expectedCentroid, barOut),
        "zeroed MV must not pass the midpoint verdict");
    if (scaled) {
        expect(!accepts_midpoint(measure_midpoint(xPrevOut, arm.stepOut, barOut, -8.0), expectedCentroid, barOut),
            "unscaled 1440p MV must not pass the scaled midpoint");
    }
    std::printf("PASS mv_sign_oracle %s mv=%+.4f midpoint=%.3f expected=%.3f hit_rate=%.4f naNs=0 "
                "sr_ofa_exec_count=0 fg_ofa_exec_count=0\n",
        arm.name, sample.x, honest.centroid, expectedCentroid, hitRate);
    expect(shared->close() && local->closes == 1, "shared provider close");
}

// A producer that forgets the extent ratio publishes the analysis-pixel vector
// verbatim; the real consumer path must then miss the scaled midpoint.
void wrong_scale_lane(Device& gpu, const Arm& arm)
{
    auto motion = texture(gpu, arm.guide, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON);
    fill_motion(gpu, motion.Get(), arm.guide, -kStepPx, 0.0f, false);
    auto counting = std::make_unique<CountingGuides>();
    auto* local = counting.get();
    auto shared = make_shared_anr_guides(gpu.device.Get(), arm.render, default_frame_interval_ns, std::move(counting));
    ImportedFlow flow{};
    flow.motion = motion.Get();
    flow.guideWidth = arm.guide.width;
    flow.guideHeight = arm.guide.height;
    flow.mvScaleX = flow.mvScaleY = 1.0f; // the defect under test
    flow.owner = ipc::FlowOwner::NrOwned;
    flow.provenance = ipc::FlowProvenance::Valid;
    shared->configure(flow);
    FlowProblem problem = FlowProblem::InvalidKey;
    const FlowImage image{key_of(11), nullptr, NV_OF_FENCE_POINT{}};
    expect(shared->shared_path(gpu.list.Get(), image, false, problem) && problem == FlowProblem::None,
        "unscaled lane takes the shared path");
    gpu.execute();
    const auto sample = read_motion(gpu, shared->result()->motion);
    expect(std::fabs(sample.x + kStepPx) < 0.02f, "unscaled lane keeps the raw analysis vector");
    const int barOut = static_cast<int>(std::lround(120.0 * arm.render.width / arm.guide.width));
    const double xPrevOut = 320.0 * arm.render.width / arm.guide.width;
    const auto verdict = measure_midpoint(xPrevOut, arm.stepOut, barOut, sample.x);
    expect(!accepts_midpoint(verdict, xPrevOut + barOut / 2.0 + arm.expectMidDelta, barOut),
        "unscaled 1440p MV must not pass the scaled midpoint");
    std::printf("PASS mv_sign_oracle unscaled-rejected mv=%+.4f centroid=%.3f\n", sample.x, verdict.centroid);
    expect(shared->close() && local->closes == 1, "unscaled provider close");
}

// An unclean export border must be neutralized by the consumer scale pass.
void sanitize_lane(Device& gpu, const Arm& arm)
{
    auto motion = texture(gpu, arm.guide, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COMMON);
    fill_motion(gpu, motion.Get(), arm.guide, -kStepPx, 0.0f, true);
    auto counting = std::make_unique<CountingGuides>();
    auto* local = counting.get();
    auto shared = make_shared_anr_guides(gpu.device.Get(), arm.render, default_frame_interval_ns, std::move(counting));
    ImportedFlow flow{};
    flow.motion = motion.Get();
    flow.guideWidth = arm.guide.width;
    flow.guideHeight = arm.guide.height;
    flow.owner = ipc::FlowOwner::NrOwned;
    flow.provenance = ipc::FlowProvenance::Valid;
    shared->configure(flow);
    FlowProblem problem = FlowProblem::InvalidKey;
    const FlowImage image{key_of(9), nullptr, NV_OF_FENCE_POINT{}};
    expect(shared->shared_path(gpu.list.Get(), image, false, problem), "sanitize lane takes the shared path");
    expect(problem == FlowProblem::None && shared->result() && shared->result()->motion, "sanitize lane result");
    gpu.execute();
    const auto sample = read_motion(gpu, shared->result()->motion);
    expect(sample.texels > 0 && sample.nanTexels == 0, "consumer MV texture still contains NaNs after sanitization");
    std::printf("PASS mv_sign_oracle sanitize naNs=0 texels=%zu\n", sample.texels);
    expect(shared->close() && local->closes == 1, "sanitize provider close");
}

// NR removed or disabled: the consumer owns analysis again.
void nr_off_lane(Device& gpu, const Arm& arm)
{
    auto counting = std::make_unique<CountingGuides>();
    auto* local = counting.get();
    auto shared = make_shared_anr_guides(gpu.device.Get(), arm.render, default_frame_interval_ns, std::move(counting));
    shared->configure(ImportedFlow{nullptr, 0, 0, 1.0f, 1.0f, ipc::FlowOwner::ConsumerFallback,
        ipc::FlowProvenance::Unknown, {}, false, false});
    const auto image = texture(gpu, arm.guide, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_STATE_COMMON);
    for (int frame = 0; frame < 4; ++frame) {
        const auto key = key_of(static_cast<uint64_t>(100 + frame));
        expect(shared->submit(FlowInput{FlowImage{key, image.Get(), NV_OF_FENCE_POINT{}}, nullptr, arm.guide,
            {0, 0, arm.guide.width, arm.guide.height}, false}) == FlowProblem::None,
            "ConsumerFallback submission failed");
    }
    expect(shared->stats().ofaExecutions == 4 && local->submits == 4, "fg_ofa_exec_count must be > 0 when NR is off");
    SrFlowInput sr{};
    sr.nrPresent = true;
    sr.nrEnabled = false;
    sr.owner = FlowOwnerPolicy::ConsumerFallback;
    sr.provenance = nr::FlowProvenance::Unknown;
    SrFlowCounters counters;
    for (int frame = 0; frame < 4; ++frame) {
        const auto mode = sr_flow_mode(sr);
        expect(mode == SrFlowMode::LocalFallback, "disabled NR must allow the consumer-local OFA");
        counters.observe(mode, true);
    }
    expect(counters.ofaExec == 4, "sr_ofa_exec_count must be > 0 when NR is off");
    std::printf("PASS mv_sign_oracle nr-off fg_ofa_exec_count=%llu sr_ofa_exec_count=%llu\n",
        static_cast<unsigned long long>(shared->stats().ofaExecutions),
        static_cast<unsigned long long>(counters.ofaExec));
    expect(shared->close() && local->closes == 1, "nr-off provider close");
}
} // namespace

int main()
{
    try {
        char flag[8]{};
        const bool invert = GetEnvironmentVariableA("OBS_NR_TEST_INVERT_MV", flag, sizeof flag) > 0;
        Device gpu;
        if (!gpu.open()) {
            std::puts("SKIP mv_sign_oracle: no hardware D3D12 adapter");
            return 0;
        }
        const Arm native{"native-1440p", {2560, 1440}, {2560, 1440}, kStepPx,
            -static_cast<double>(kStepPx), static_cast<double>(kStepPx) * 0.5};
        const Arm scaled{"1080p-to-1440p", {1920, 1080}, {2560, 1440},
            static_cast<double>(kStepPx) * 4.0 / 3.0, -static_cast<double>(kStepPx) * 4.0 / 3.0,
            static_cast<double>(kStepPx) * 4.0 / 3.0 * 0.5};
        run_arm(gpu, native, invert);
        run_arm(gpu, scaled, invert);
        wrong_scale_lane(gpu, scaled);
        sanitize_lane(gpu, scaled);
        nr_off_lane(gpu, native);
        std::printf("PASS mv_sign_oracle checks=%u\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL mv_sign_oracle: %s\n", error.what());
        return 1;
    }
}
