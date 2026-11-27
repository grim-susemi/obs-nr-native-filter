// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - one QA host owns the CPU flow contract and the opt-in
// fence-owned shared-ring lane; CMake registers a single flow_contract target.
#include "nr_guides.h"
#include "nr_flow_broker.h"
#include "nr_flow_scenario.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace nr;
using Microsoft::WRL::ComPtr;
static unsigned checks = 0;
static void require(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }

namespace {
void gpu_checked(HRESULT result, const char* why) { require(SUCCEEDED(result), why); }
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}
D3D12_RESOURCE_DESC texture_description(Extent extent)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = extent.width;
    description.Height = extent.height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R16G16_FLOAT;
    description.SampleDesc.Count = 1;
    return description;
}
ComPtr<ID3D12Resource> texture(ID3D12Device* device, Extent extent)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto description = texture_description(extent);
    ComPtr<ID3D12Resource> result;
    gpu_checked(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result)), "QA motion texture");
    return result;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    gpu_checked(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&result)), "QA buffer");
    return result;
}
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
bool nan_half(uint16_t value) { return (value & 0x7c00u) == 0x7c00u && (value & 0x3ffu) != 0; }
int64_t qpc_now()
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}
double qpc_ms(int64_t begin, int64_t end)
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? 1000.0 * static_cast<double>(end - begin) / static_cast<double>(frequency.QuadPart) : 0.0;
}
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t serial = 0;
    Gpu() = default;
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;
    ~Gpu() { if (event) CloseHandle(event); }
    void init(IDXGIAdapter1* adapter)
    {
        gpu_checked(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "QA D3D12 device");
        D3D12_COMMAND_QUEUE_DESC description{};
        description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        gpu_checked(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)), "QA queue");
        gpu_checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
            "QA allocator");
        gpu_checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)), "QA list");
        gpu_checked(list->Close(), "QA initial list close");
        gpu_checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "QA fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "QA completion event");
    }
    void begin(const char* where)
    {
        const auto reset = allocator->Reset();
        if (FAILED(reset)) {
            char message[192];
            std::snprintf(message, sizeof message, "QA allocator reset at %s hr=0x%08x removed=0x%08x", where,
                static_cast<unsigned>(reset), static_cast<unsigned>(device->GetDeviceRemovedReason()));
            throw std::runtime_error(message);
        }
        gpu_checked(list->Reset(allocator.Get(), nullptr), "QA list reset");
    }
    void flush()
    {
        gpu_checked(list->Close(), "QA list close");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
    }
    void wait()
    {
        require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "QA unresolved GPU completion");
        require(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= serial,
            "QA fence removed");
    }
    void sync()
    {
        const auto next = ++serial;
        gpu_checked(fence->SetEventOnCompletion(next, event), "QA subscribe completion");
        gpu_checked(queue->Signal(fence.Get(), next), "QA completion signal");
        wait();
    }
    void signal_shared(ID3D12Fence* shared, uint64_t value)
    {
        const auto next = ++serial;
        gpu_checked(fence->SetEventOnCompletion(next, event), "QA subscribe shared release");
        gpu_checked(queue->Signal(shared, value), "QA shared return signal");
        gpu_checked(queue->Signal(fence.Get(), next), "QA shared release order");
        wait();
    }
    void signal_many(const std::vector<ID3D12Fence*>& shared, const std::vector<uint64_t>& values)
    {
        require(shared.size() == values.size(), "QA batch release arity");
        const auto next = ++serial;
        gpu_checked(fence->SetEventOnCompletion(next, event), "QA subscribe batch release");
        for (size_t index = 0; index < shared.size(); ++index) queue->Signal(shared[index], values[index]);
        gpu_checked(queue->Signal(fence.Get(), next), "QA batch release order");
        wait();
    }
};
void fill_motion(ID3D12Resource* upload, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
    Extent extent, Rect active, bool nanOutside)
{
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    gpu_checked(upload->Map(0, &none, &mapped), "QA source map");
    auto* pixels = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < extent.height; ++y) for (uint32_t x = 0; x < extent.width; ++x) {
        const bool inside = x >= active.x && y >= active.y && x < active.x + active.width &&
            y < active.y + active.height;
        uint16_t value[2]{};
        if (!inside && nanOutside && x < active.x + active.width / 2) {
            value[0] = 0x7fc0u; // private guide marker: NaN outside the active rectangle
            value[1] = 0x7fdeu;
        } else if (!inside) {
            value[0] = half_of(3.0f); // finite leakage the export must still zero
            value[1] = half_of(-2.0f);
        } else if (inside) {
            value[0] = half_of(static_cast<float>(x - active.x) * 0.25f);
            value[1] = half_of(-static_cast<float>(y - active.y) * 0.5f);
        }
        std::memcpy(pixels + static_cast<size_t>(footprint.Offset) +
            static_cast<size_t>(y) * footprint.Footprint.RowPitch + static_cast<size_t>(x) * 4, value, 4);
    }
    upload->Unmap(0, nullptr);
}
void verify_motion(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, Extent extent, Rect active,
    const void* mapped, bool expectSanitized)
{
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    uint64_t outsideNonZero = 0, outsideNan = 0, insideMismatch = 0, insideCompared = 0;
    for (uint32_t y = 0; y < extent.height; ++y) for (uint32_t x = 0; x < extent.width; ++x) {
        uint16_t value[2]{};
        std::memcpy(value, pixels + static_cast<size_t>(footprint.Offset) +
            static_cast<size_t>(y) * footprint.Footprint.RowPitch + static_cast<size_t>(x) * 4, 4);
        const bool inside = x >= active.x && y >= active.y && x < active.x + active.width &&
            y < active.y + active.height;
        if (!inside) {
            if (nan_half(value[0]) || nan_half(value[1])) ++outsideNan;
            if (value[0] || value[1]) ++outsideNonZero;
            continue;
        }
        const auto expectedX = half_of(static_cast<float>(x - active.x) * 0.25f);
        const auto expectedY = half_of(-static_cast<float>(y - active.y) * 0.5f);
        if (value[0] != expectedX || value[1] != expectedY) {
            if (insideMismatch < 5)
                std::printf("MISMATCH x=%u y=%u got=(0x%04x,0x%04x) expected=(0x%04x,0x%04x)\n",
                    x, y, value[0], value[1], expectedX, expectedY);
            ++insideMismatch;
        }
        ++insideCompared;
    }
    require(outsideNan == 0, "exported ring kept a NaN outside activeRect");
    require(outsideNonZero == 0, "exported ring is not zero outside activeRect");
    require(insideMismatch == 0, "exported ring changed the analysed vectors");
    require(insideCompared == uint64_t{active.width} * active.height || !expectSanitized,
        "exported ring active region size");
}
JobKey job_key(uint64_t instance, uint64_t sequence, uint64_t serial)
{
    return JobKey{{instance, 1, sequence}, 1, 1, serial};
}
FramePairKey pair_key(uint64_t instance, uint64_t previousTick, uint64_t currentTick,
    uint64_t previousSerial, uint64_t currentSerial)
{
    return FramePairKey{{instance, previousTick, 1, 1, previousSerial, 1},
        {instance, currentTick, 1, 1, currentSerial, 1}};
}
void gpu_flow_broker()
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::printf("SKIP flow broker gpu: no DXGI factory\n");
        return;
    }
    ComPtr<IDXGIAdapter1> chosen;
    for (UINT index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        ComPtr<ID3D12Device> probe;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&probe)))) continue;
        chosen = adapter;
        if (description.VendorId == 0x10de) break;
    }
    if (!chosen) {
        std::printf("SKIP flow broker gpu: no hardware D3D12 adapter\n");
        return;
    }
    Gpu owner, observer;
    owner.init(chosen.Get());
    observer.init(chosen.Get());
    const Extent extent{320, 192};
    const Rect active{8, 8, 304, 176};
    constexpr uint64_t instance = 0x51u;
    auto source = texture(owner.device.Get(), extent);
    const auto sourceDescription = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT sourceFootprint{};
    UINT64 sourceBytes = 0;
    owner.device->GetCopyableFootprints(&sourceDescription, 0, 1, 0, &sourceFootprint, nullptr, nullptr, &sourceBytes);
    auto upload = buffer(owner.device.Get(), sourceBytes, D3D12_HEAP_TYPE_UPLOAD);
    fill_motion(upload.Get(), sourceFootprint, extent, active, true);
    owner.begin("source fill");
    transition(owner.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = upload.Get();
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint = sourceFootprint;
    D3D12_TEXTURE_COPY_LOCATION textureLocation{};
    textureLocation.pResource = source.Get();
    textureLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    owner.list->CopyTextureRegion(&textureLocation, 0, 0, 0, &sourceLocation, nullptr);
    transition(owner.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    owner.flush();
    owner.sync();
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT checkFootprint{};
        UINT64 checkBytes = 0;
        owner.device->GetCopyableFootprints(&sourceDescription, 0, 1, 0, &checkFootprint, nullptr, nullptr, &checkBytes);
        auto check = buffer(owner.device.Get(), checkBytes, D3D12_HEAP_TYPE_READBACK);
        owner.begin("source readback");
        transition(owner.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION checkDestination{};
        checkDestination.pResource = check.Get();
        checkDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        checkDestination.PlacedFootprint = checkFootprint;
        D3D12_TEXTURE_COPY_LOCATION checkOrigin{};
        checkOrigin.pResource = source.Get();
        checkOrigin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        owner.list->CopyTextureRegion(&checkDestination, 0, 0, 0, &checkOrigin, nullptr);
        transition(owner.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        owner.flush();
        owner.sync();
        void* checkMapped = nullptr;
        gpu_checked(check->Map(0, nullptr, &checkMapped), "QA source readback");
        const auto* pixels = static_cast<const uint8_t*>(checkMapped);
        unsigned nonZero = 0, nan = 0;
        for (uint32_t y = 0; y < extent.height; ++y) for (uint32_t x = 0; x < extent.width; ++x) {
            uint16_t value[2]{};
            std::memcpy(value, pixels + static_cast<size_t>(checkFootprint.Offset) +
                static_cast<size_t>(y) * checkFootprint.Footprint.RowPitch + static_cast<size_t>(x) * 4, 4);
            if (value[0] || value[1]) ++nonZero;
            if (nan_half(value[0]) || nan_half(value[1])) ++nan;
        }
        check->Unmap(0, nullptr);
        std::printf("SOURCE fixture nonzero=%u nan=%u (%ux%u)\n", nonZero, nan, extent.width, extent.height);
    }
    const SharedFlowSlot guide{extent.width, extent.height, active.x, active.y, active.width, active.height,
        1.0f, 1.0f, false, false};
    std::string diagnostic;
    auto broker = FlowBroker::create({owner.device.Get(), owner.queue.Get(), extent, instance}, &diagnostic);
    require(static_cast<bool>(broker), diagnostic.empty() ? "flow broker unavailable" : diagnostic.c_str());
    require(broker->slot_count() == flow_ring_slots, "bounded four-slot ring");
    require(find_flow_broker(instance, 1).get() == broker.get(), "broker registry lookup by source generation");
    FramePairKey lastPair{};
    for (uint32_t index = 0; index < 8; ++index) {
        const uint64_t previousTick = 21 + index, currentTick = 22 + index;
        const auto previous = job_key(instance, index + 1, 100 + index);
        const auto current = job_key(instance, index + 2, 200 + index);
        const FlowExport flow{FlowFrameRef{current, currentTick}, FlowFrameRef{previous, previousTick},
            source.Get(), guide};
        owner.begin("export");
        require(broker->record_export(owner.list.Get(), flow), "ring export admission");
        owner.flush();
        broker->commit_export(owner.queue.Get());
        owner.sync();
        lastPair = pair_key(instance, previousTick, currentTick, 100 + index, 200 + index);
        require(broker->pair_for_job(1, currentTick + 1, 200 + index) == lastPair,
            "exported pair is keyed by its displayed tick");
    }
    require(broker->stats().exported == 8 && broker->stats().droppedExport == 0, "eight sequential exports");
    const auto lease = broker->find_pair(lastPair, FlowConsumer::Sr);
    require(lease.has_value(), "exported pair lease");
    ComPtr<ID3D12Resource> remote;
    ComPtr<ID3D12Fence> readyFence, returnFence;
    gpu_checked(observer.device->OpenSharedHandle(lease->motion, IID_PPV_ARGS(&remote)),
        "NT motion handle opens on a separate device");
    gpu_checked(observer.device->OpenSharedHandle(lease->readyFence, IID_PPV_ARGS(&readyFence)),
        "ready fence opens on a separate device");
    gpu_checked(observer.device->OpenSharedHandle(lease->returnFence, IID_PPV_ARGS(&returnFence)),
        "return fence opens on a separate device");
    require(readyFence->GetCompletedValue() >= lease->readyValue, "ready fence is signalled");
    require(remote->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT, "exported format is R16G16_FLOAT");
    require(remote->GetDesc().Width == extent.width && remote->GetDesc().Height == extent.height,
        "exported extent is the analysis extent");
    const auto remoteDescription = remote->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readFootprint{};
    UINT64 readBytes = 0;
    observer.device->GetCopyableFootprints(&remoteDescription, 0, 1, 0, &readFootprint, nullptr, nullptr, &readBytes);
    auto readback = buffer(observer.device.Get(), readBytes, D3D12_HEAP_TYPE_READBACK);
    observer.begin("observer readback");
    transition(observer.list.Get(), remote.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = readFootprint;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = remote.Get();
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    observer.list->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
    transition(observer.list.Get(), remote.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    observer.flush();
    observer.sync();
    void* mapped = nullptr;
    gpu_checked(readback->Map(0, nullptr, &mapped), "QA exported readback");
    verify_motion(readFootprint, extent, active, mapped, true);
    readback->Unmap(0, nullptr);
    observer.signal_shared(returnFence.Get(), lease->returnValue);
    const auto displayed = classify_display(DisplayInput{OutputKind::DelayedNr, Status::Applied, true,
        false, false, false, lastPair});
    require(displayed.owner == FlowOwnerPolicy::NrOwned && displayed.provenance == FlowProvenance::Valid,
        "displayed DelayedNr maps to the exact analysed pair");
    require(FlowBroker::display_tick_for(lastPair, lastPair.currColorKey.canonicalTick + 1),
        "displayed tick T maps to the pair ending at T-1");
    broker->record_display(1, lastPair.currColorKey.canonicalTick + 1, displayed);
    require(broker->displayed(1, lastPair.currColorKey.canonicalTick + 1) == displayed, "display provenance record");
    require(broker->close(), "ring closes after every leased consumer returned");

    std::string secondDiagnostic;
    auto second = FlowBroker::create({owner.device.Get(), owner.queue.Get(), extent, instance + 1}, &secondDiagnostic);
    require(static_cast<bool>(second), secondDiagnostic.empty() ? "second broker unavailable" : secondDiagnostic.c_str());
    std::array<std::array<ExportedFlow, 2>, flow_ring_slots> pinned{};
    for (uint32_t index = 0; index < flow_ring_slots; ++index) {
        const uint64_t previousTick = 41 + index, currentTick = 42 + index;
        const auto previous = job_key(instance + 1, index + 1, 300 + index);
        const auto current = job_key(instance + 1, index + 2, 400 + index);
        const FlowExport flow{FlowFrameRef{current, currentTick}, FlowFrameRef{previous, previousTick},
            source.Get(), guide};
        owner.begin("pin");
        require(second->record_export(owner.list.Get(), flow), "pin export admission");
        owner.flush();
        second->commit_export(owner.queue.Get());
        owner.sync();
        const auto pinnedPair = pair_key(instance + 1, previousTick, currentTick, 300 + index, 400 + index);
        const auto leaseSr = second->find_pair(pinnedPair, FlowConsumer::Sr);
        require(leaseSr.has_value() && leaseSr->slot == index, "pinned slot index");
        pinned[index][0] = *leaseSr;
        const auto leaseFg = second->find_pair(pinnedPair, FlowConsumer::Fg);
        require(leaseFg.has_value() && leaseFg->slot == index, "pinned FG slot");
        pinned[index][1] = *leaseFg;
    }
    const auto blockedPrevious = job_key(instance + 1, 6, 306);
    const auto blockedCurrent = job_key(instance + 1, 7, 406);
    const FlowExport blocked{FlowFrameRef{blockedCurrent, 46}, FlowFrameRef{blockedPrevious, 45},
        source.Get(), guide};
    owner.begin("blocked");
    const auto begin = qpc_now();
    const bool admitted = second->record_export(owner.list.Get(), blocked);
    const auto end = qpc_now();
    owner.flush(); // a full ring records nothing; close the empty list before reuse
    owner.sync();
    require(!admitted, "ring-full export must not be admitted");
    require(second->stats().droppedExport == 1, "ring-full export counted once");
    require(qpc_ms(begin, end) < 100.0, "ring-full export must not block the worker");
    const auto blockedPair = pair_key(instance + 1, 45, 46, 306, 406);
    require(!second->find_pair(blockedPair, FlowConsumer::Sr), "dropped pair never exposed");
    require(!second->pair_for_job(1, 47, 406), "dropped pair has no display mapping");
    const auto held = classify_display(DisplayInput{OutputKind::DelayedRaw, Status::Bypassed, true,
        false, true, false, std::nullopt});
    require(held.owner == FlowOwnerPolicy::NrOwned && held.provenance == FlowProvenance::HeldPrevious,
        "held frame reports HeldPrevious");
    const auto bypassed = classify_display(DisplayInput{OutputKind::CurrentSource, Status::Bypassed, true,
        true, false, false, std::nullopt});
    require(bypassed.owner != FlowOwnerPolicy::NrOwned || bypassed.provenance != FlowProvenance::Valid,
        "bypassed frame never reports valid motion");
    const auto missing = classify_display(DisplayInput{OutputKind::DelayedNr, Status::Applied, true,
        false, false, false, std::nullopt});
    require(missing.provenance == FlowProvenance::Discontinuity, "DelayedNr without a pair is a discontinuity");
    std::vector<ComPtr<ID3D12Fence>> opened;
    std::vector<uint64_t> values;
    for (const auto& slot : pinned) for (const auto& entry : slot) {
        ComPtr<ID3D12Fence> fence;
        gpu_checked(observer.device->OpenSharedHandle(entry.returnFence, IID_PPV_ARGS(&fence)), "open return fence");
        opened.push_back(fence);
        values.push_back(entry.returnValue);
    }
    std::vector<ID3D12Fence*> raw;
    for (auto& fence : opened) raw.push_back(fence.Get());
    observer.signal_many(raw, values);
    const auto recoveredPrevious = job_key(instance + 1, 8, 308);
    const auto recoveredCurrent = job_key(instance + 1, 9, 408);
    const FlowExport recovered{FlowFrameRef{recoveredCurrent, 48}, FlowFrameRef{recoveredPrevious, 47},
        source.Get(), guide};
    owner.begin("recovered");
    require(second->record_export(owner.list.Get(), recovered), "returned leases release a slot");
    owner.flush();
    second->commit_export(owner.queue.Get());
    owner.sync();
    require(second->stats().reusedSlots >= 1, "slot recycled only after consumer fences completed");
    const auto recoveredPair = pair_key(instance + 1, 47, 48, 308, 408);
    const auto recoveredLease = second->find_pair(recoveredPair, FlowConsumer::Fg);
    require(recoveredLease.has_value(), "recycled pair exposed");
    ComPtr<ID3D12Fence> recoveredFence;
    gpu_checked(observer.device->OpenSharedHandle(recoveredLease->returnFence, IID_PPV_ARGS(&recoveredFence)),
        "open recycled return fence");
    observer.signal_shared(recoveredFence.Get(), recoveredLease->returnValue);
    require(second->close(), "ring closes after all recovered consumers returned");

    // Teardown with an outstanding lease: the ring is parked, never freed, and
    // the next generation still fits the two-entry retirement budget.
    std::string parkedDiagnostic, nextDiagnostic;
    auto parked = FlowBroker::create({owner.device.Get(), owner.queue.Get(), extent, instance + 2},
        &parkedDiagnostic);
    require(static_cast<bool>(parked), parkedDiagnostic.empty() ? "parked broker unavailable" : parkedDiagnostic.c_str());
    const auto parkedPrevious = job_key(instance + 2, 1, 499);
    const auto parkedCurrent = job_key(instance + 2, 2, 500);
    const FlowExport parkedFlow{FlowFrameRef{parkedCurrent, 61}, FlowFrameRef{parkedPrevious, 60},
        source.Get(), guide};
    owner.begin("parked");
    require(parked->record_export(owner.list.Get(), parkedFlow), "parked export admission");
    owner.flush();
    parked->commit_export(owner.queue.Get());
    owner.sync();
    const auto parkedPair = pair_key(instance + 2, 60, 61, 499, 500);
    require(parked->find_pair(parkedPair, FlowConsumer::Sr).has_value(), "parked lease");
    parked.reset(); // the lease is still open: unresolved ownership is parked, not freed
    auto next = FlowBroker::create({owner.device.Get(), owner.queue.Get(), extent, instance + 3},
        &nextDiagnostic);
    require(static_cast<bool>(next), nextDiagnostic.empty() ? "retirement capacity" : nextDiagnostic.c_str());
    const auto nextPrevious = job_key(instance + 3, 1, 501);
    const auto nextCurrent = job_key(instance + 3, 2, 502);
    const FlowExport nextFlow{FlowFrameRef{nextCurrent, 63}, FlowFrameRef{nextPrevious, 62},
        source.Get(), guide};
    owner.begin("next generation");
    require(next->record_export(owner.list.Get(), nextFlow), "next generation export");
    owner.flush();
    next->commit_export(owner.queue.Get());
    owner.sync();
    require(next->close(), "next generation closes without leases");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const bool gpu = argc == 2 && std::string(argv[1]) == "--gpu";
        require((argc == 1 || gpu), "malformed invocation");
        std::ostringstream admission;
        require(nr::qa::run_flow_translation(nullptr, nullptr, {}, false, admission) == 2,
            "CPU-only guard must refuse before device/API access");
        require(admission.str().find("CPU_READY_HARDWARE_NOT_AUTHORIZED") != std::string::npos,
            "CPU admission marker");
        require(!smallest_flow_grid({}), "empty capability accepted");
        const std::array<uint32_t, 3> caps{4, 1, 2};
        require(smallest_flow_grid(caps) == 1u, "smallest queried grid");
        const std::array<uint32_t, 1> wrong{8};
        require(!smallest_flow_grid(wrong), "unknown SDK grid accepted");
        for (auto width : {1920u, 2560u}) {
            const Extent size{width, width == 1920 ? 1080u : 1440u};
            const Rect full{0, 0, size.width, size.height};
            for (auto amount : {1, 8, 32}) for (auto sign : {-1, 1}) for (auto axis : {0, 1}) {
                const auto value = decode_flow(static_cast<int16_t>(axis == 0 ? sign * amount * 32 : 0),
                    static_cast<int16_t>(axis == 1 ? sign * amount * 32 : 0));
                require(value.x == (axis == 0 ? static_cast<float>(sign * amount) : 0.f) &&
                    value.y == (axis == 1 ? static_cast<float>(sign * amount) : 0.f), "signed10.5 units/direction");
                require(flow_geometry_valid(size, full), "1080/1440 full geometry");
            }
            const Rect sub{16, 24, size.width - 32, size.height - 48};
            require(flow_geometry_valid(size, sub), "active subrect");
            require(decode_flow(-1, 1).x == -0.03125f && decode_flow(-1, 1).y == 0.03125f, "fractional signed units");
        }
        require(!flow_geometry_valid({1920, 1080}, {1919, 0, 2, 1}), "out of range rectangle");
        const JobKey prev{{41, 2, 10}, 3, 4, 11}, current{{41, 2, 11}, 3, 4, 12};
        require(flow_pair_problem(current, &prev, false) == FlowProblem::None, "current/reference chronology");
        require(flow_pair_problem(current, nullptr, true) == FlowProblem::None, "cold reset admission");
        require(flow_pair_problem(current, nullptr, false) == FlowProblem::MissingPrevious, "ordinary zero-flow rejected");
        require(flow_pair_problem(current, &prev, true) == FlowProblem::ResetPair, "reset cannot reuse old guides");
        const JobKey old{{41, 2, 10}, 2, 4, 11}, future{{41, 2, 12}, 3, 4, 11};
        require(flow_pair_problem(current, &old, false) == FlowProblem::KeyMismatch, "old guide epoch rejected");
        require(flow_pair_problem(current, &future, false) == FlowProblem::KeyMismatch, "future reference rejected");
        require(!flow_cost_format_valid(DXGI_FORMAT_R32_FLOAT), "cost is not probability");
        require(flow_cost_format_valid(DXGI_FORMAT_R8_UINT), "queried cost format");
        const Rect active{16, 24, 1888, 1032}, other{0, 0, 1920, 1080};
        require(flow_chronology_problem(current, &prev, &prev, active, &active, false) == FlowProblem::None, "cached chronology");
        require(flow_chronology_problem(current, &prev, &prev, other, &active, false) == FlowProblem::KeyMismatch,
            "active rectangle changed without reset");
        require(flow_chronology_problem(prev, nullptr, &prev, active, &active, true) == FlowProblem::KeyMismatch,
            "repeated reset key");
        const JobKey resetKey{{41, 3, 12}, 4, 4, 13};
        require(flow_chronology_problem(resetKey, nullptr, &current, active, &active, true) == FlowProblem::None,
            "cut reset advances epoch");
        const JobKey dropped{{41, 2, 13}, 3, 4, 14}, backwards{{41, 2, 13}, 2, 4, 14};
        require(flow_chronology_problem(dropped, nullptr, &current, active, &active, true) == FlowProblem::None,
            "model reset inside history epoch after dropped frame");
        require(flow_chronology_problem(backwards, nullptr, &current, active, &active, true) == FlowProblem::ResetPair,
            "reset cannot move history epoch backwards");
        require(flow_chronology_problem(current, &prev, nullptr, active, nullptr, false) == FlowProblem::MissingPrevious,
            "resume cannot infer temporal state");
        const auto init = flow_init_params({1920, 1080}, 1);
        require(init.width == 1920 && init.height == 1080 && init.outGridSize == NV_OF_OUTPUT_VECTOR_GRID_SIZE_1,
            "full working-size SDK init");
        require(init.predDirection == NV_OF_PRED_DIRECTION_FORWARD && init.enableOutputCost == NV_OF_TRUE &&
            init.enableExternalHints == NV_OF_FALSE && init.mode == NV_OF_MODE_OPTICALFLOW,
            "SDK direction/cost/no fallback");
        const auto c = reinterpret_cast<NvOFGPUBufferHandle>(uintptr_t(1));
        const auto p = reinterpret_cast<NvOFGPUBufferHandle>(uintptr_t(2));
        std::array<NV_OF_FENCE_POINT, 1> deps{{{nullptr, 7}}};
        const auto packet = flow_execute_input(c, p, true, deps);
        require(packet.inputFrame == c && packet.referenceFrame == p, "production packet current/reference order");
        require(packet.disableTemporalHints == NV_OF_TRUE && packet.numFencePoints == 1 && packet.fencePoint == deps.data(),
            "reset/dependency packet");
        require(packet.padding == 0 && packet.padding2 == 0 && packet.padding3 == 0 && !packet.hPrivData &&
            !packet.externalHints && !packet.numRois && !packet.roiData, "reserved API fields zero");
        require(flow_execute_input(c, p, false, deps).disableTemporalHints == NV_OF_FALSE, "ordinary temporal hints");
        const std::array<uint32_t, 4> hintCaps{8, 4, 1, 2};
        require(smallest_hint_grid(hintCaps, 1) == 1u && smallest_hint_grid(hintCaps, 4) == 4u,
            "admitted hint grid/output ordering");
        require(!smallest_hint_grid({}, 1) && !smallest_hint_grid(wrong, 16), "missing or too-small hint grid");
        const std::array<uint32_t, 1> invalidHint{3};
        require(!smallest_hint_grid(invalidHint, 1), "unknown hint grid");
        const auto bootstrap = flow_bootstrap_params({2560, 1440}, 1), refine = flow_refinement_params({2560, 1440}, 1, 1);
        require(bootstrap.enableGlobalFlow == NV_OF_TRUE && bootstrap.enableExternalHints == NV_OF_FALSE,
            "bootstrap estimates native global, no external predictor");
        require(refine.enableExternalHints == NV_OF_TRUE && refine.enableGlobalFlow == NV_OF_FALSE &&
            refine.hintGridSize == NV_OF_HINT_VECTOR_GRID_SIZE_1,
            "refinement admits exact hint grid, distinct pass");
        require(bootstrap.perfLevel == refine.perfLevel && bootstrap.outGridSize == refine.outGridSize &&
            refine.width == 2560 && refine.height == 1440, "uniform full-resolution native estimator");
        require(flow_init_params({1920, 1080}, 1).perfLevel == NV_OF_PERF_LEVEL_MEDIUM &&
            flow_bootstrap_params({1920, 1080}, 1, NV_OF_PERF_LEVEL_FAST).perfLevel == NV_OF_PERF_LEVEL_FAST &&
            flow_refinement_params({1920, 1080}, 4, 4, NV_OF_PERF_LEVEL_FAST).perfLevel == NV_OF_PERF_LEVEL_FAST,
            "explicit estimator speed level");
        require(flow_policy({2528, 1369}, 0).perf == NV_OF_PERF_LEVEL_MEDIUM && flow_policy({2528, 1369}, 0).minGrid == 1,
            "unknown frame rate keeps MEDIUM full grid");
        require(flow_policy({960, 540}, 16666667).perf == NV_OF_PERF_LEVEL_MEDIUM &&
            flow_policy({960, 540}, 16666667).minGrid == 1, "small pixel rate keeps MEDIUM");
        require(flow_policy({1478, 919}, 16666667).perf == NV_OF_PERF_LEVEL_FAST &&
            flow_policy({1478, 919}, 16666667).minGrid == 1, "measured 1478x919@60 FAST full grid");
        require(flow_policy({2528, 1369}, 33333333).perf == NV_OF_PERF_LEVEL_FAST &&
            flow_policy({2528, 1369}, 33333333).minGrid == 1, "measured 2528x1369@30 FAST full grid");
        require(flow_policy({2560, 1440}, 16666667).perf == NV_OF_PERF_LEVEL_FAST &&
            flow_policy({2560, 1440}, 16666667).minGrid == 4, "owner 1440p@60 FAST 4-pixel grid");
        const auto hint = reinterpret_cast<NvOFGPUBufferHandle>(uintptr_t(3));
        const auto refinePacket = flow_execute_input(c, p, true, deps, hint);
        require(refinePacket.externalHints == hint && refinePacket.disableTemporalHints == NV_OF_TRUE &&
            refinePacket.fencePoint == deps.data(),
            "refinement uses this-pair GPU hint, never prior-call temporal field");

        const FlowColorKey previousColor{7, 11, 3, 1, 21, 4};
        const FlowColorKey currentColor{7, 12, 3, 1, 22, 4};
        const FramePairKey pair{previousColor, currentColor};
        require(pair.valid(), "adjacent pair identity");
        require(FlowBroker::display_tick_for(pair, 13) && !FlowBroker::display_tick_for(pair, 14),
            "display tick is one after the analysed frame");
        require(!FramePairKey{previousColor, FlowColorKey{7, 12, 3, 2, 22, 4}}.valid(), "content epoch mismatch");
        require(!FramePairKey{previousColor, FlowColorKey{7, 14, 3, 1, 22, 4}}.valid(), "non-adjacent ticks");
        require(!FramePairKey{previousColor, FlowColorKey{8, 12, 3, 1, 22, 4}}.valid(), "source identity mismatch");
        const Extent analysis{320, 192};
        const SharedFlowSlot guide{320, 192, 8, 8, 304, 176, 1.0f, 1.0f, false, false};
        require(guide.valid(analysis), "analysis-pixel guide metadata");
        require(!SharedFlowSlot{320, 192, 8, 8, 304, 176, 1.0f, 0.0f, false, false}.valid(analysis),
            "zero motion scale rejected");
        require(!SharedFlowSlot{1920, 1080, 0, 0, 1920, 1080, 1.0f, 1.0f, false, false}.valid(analysis),
            "guide extent must equal the ring extent");
        require(!SharedFlowSlot{320, 192, 8, 8, 400, 176, 1.0f, 1.0f, false, false}.valid(analysis),
            "active rect must fit the ring");
        const auto validNr = classify_display(DisplayInput{OutputKind::DelayedNr, Status::Applied, true,
            false, false, false, pair});
        require(validNr.owner == FlowOwnerPolicy::NrOwned && validNr.provenance == FlowProvenance::Valid &&
            validNr.pair == pair, "DelayedNr with the exact pair is valid");
        const auto reused = classify_display(DisplayInput{OutputKind::DelayedNr, Status::Applied, true,
            false, false, true, pair});
        require(reused.provenance == FlowProvenance::DedupReused && !reused.pair,
            "dedup reuse is never promoted to valid motion");
        const auto unmatched = classify_display(DisplayInput{OutputKind::DelayedNr, Status::Applied, true,
            false, false, false, std::nullopt});
        require(unmatched.owner == FlowOwnerPolicy::NrOwned && unmatched.provenance == FlowProvenance::Discontinuity,
            "DelayedNr without a pair is a discontinuity");
        const auto held = classify_display(DisplayInput{OutputKind::DelayedRaw, Status::Bypassed, true,
            false, true, false, std::nullopt});
        require(held.owner == FlowOwnerPolicy::NrOwned && held.provenance == FlowProvenance::HeldPrevious,
            "held frame reports HeldPrevious");
        const auto late = classify_display(DisplayInput{OutputKind::DelayedRaw, Status::Bypassed, true,
            true, false, false, std::nullopt});
        require(late.owner != FlowOwnerPolicy::NrOwned && late.provenance == FlowProvenance::Reset,
            "late raw frame has no owner motion");
        const auto priming = classify_display(DisplayInput{OutputKind::PrimingRaw, Status::Priming, true,
            false, false, false, std::nullopt});
        require(priming.owner == FlowOwnerPolicy::Unavailable && priming.provenance == FlowProvenance::Reset,
            "priming frame resets and owns nothing");
        const auto disabled = classify_display(DisplayInput{OutputKind::CurrentSource, Status::Disabled,
            false, false, false, false, std::nullopt});
        require(disabled.owner == FlowOwnerPolicy::ConsumerFallback, "disabled NR permits consumer local OFA");
        const auto bypassed = classify_display(DisplayInput{OutputKind::CurrentSource, Status::Bypassed,
            true, true, false, false, std::nullopt});
        require(bypassed.owner == FlowOwnerPolicy::NrOwned && bypassed.provenance == FlowProvenance::Reset,
            "bypassed while NR owns the chain resets, never falls back");

        if (gpu) gpu_flow_broker();
        std::printf("PASS flow_contract %u assertions; CPU contract%s\n", checks,
            gpu ? " + shared ring GPU lane" : " only, no device/API initialization");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL flow_contract: %s\n", e.what());
        return 1;
    }
}
