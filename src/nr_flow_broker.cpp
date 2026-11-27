// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - the assigned source-only handoff permits only
// nr_flow_broker.h/.cpp; the shared ring, its fence custody and the displayed
// provenance state machine are one bounded owner and must be reviewed together.
// It contains no scheduling, presentation or vendor call.
#include "nr_flow_broker.h"
#include "nr_flow_export.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace nr {
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(what);
}
bool complete(ID3D12Fence* fence, uint64_t value)
{
    if (!fence || !value) return false;
    const auto actual = fence->GetCompletedValue();
    return actual != UINT64_MAX && actual >= value;
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}
struct RegistryEntry {
    const FlowBroker* identity = nullptr;
    std::weak_ptr<FlowBroker> broker;
};
struct Registry {
    std::mutex lock;
    std::array<RegistryEntry, 4> entries;
};
Registry& registry()
{
    static Registry value;
    return value;
}
} // namespace

bool FlowColorKey::valid() const
{
    return sourceInstanceId && canonicalTick && historyEpoch && contentEpoch &&
        submissionSerial && resourceGeneration;
}
bool FramePairKey::valid() const
{
    return prevColorKey.valid() && currColorKey.valid() &&
        prevColorKey.sourceInstanceId == currColorKey.sourceInstanceId &&
        prevColorKey.resourceGeneration == currColorKey.resourceGeneration &&
        prevColorKey.historyEpoch == currColorKey.historyEpoch &&
        prevColorKey.contentEpoch == currColorKey.contentEpoch &&
        currColorKey.canonicalTick == prevColorKey.canonicalTick + 1 &&
        currColorKey.submissionSerial > prevColorKey.submissionSerial;
}
bool SharedFlowSlot::valid(Extent analysis) const
{
    return guideWidth == analysis.width && guideHeight == analysis.height && guideWidth && guideHeight &&
        activeWidth && activeHeight && activeX <= guideWidth && activeY <= guideHeight &&
        activeWidth <= guideWidth - activeX && activeHeight <= guideHeight - activeY &&
        std::isfinite(mvScaleX) && std::isfinite(mvScaleY) && mvScaleX != 0.0f && mvScaleY != 0.0f;
}
bool FlowBroker::display_tick_for(const FramePairKey& pair, uint64_t displayTick)
{
    return pair.valid() && pair.currColorKey.canonicalTick + 1 == displayTick;
}
DisplayedFlow classify_display(const DisplayInput& input)
{
    switch (input.kind) {
    case OutputKind::DelayedNr:
        if (input.dedupReused)
            return {FlowOwnerPolicy::NrOwned, FlowProvenance::DedupReused, std::nullopt};
        if (input.pair) return {FlowOwnerPolicy::NrOwned, FlowProvenance::Valid, input.pair};
        return {FlowOwnerPolicy::NrOwned, FlowProvenance::Discontinuity, std::nullopt};
    case OutputKind::DelayedRaw:
        if (input.heldPrevious)
            return {FlowOwnerPolicy::NrOwned, FlowProvenance::HeldPrevious, std::nullopt};
        return {FlowOwnerPolicy::Unavailable,
            input.resetTemporal ? FlowProvenance::Reset : FlowProvenance::Unknown, std::nullopt};
    case OutputKind::PrimingRaw:
        return {FlowOwnerPolicy::Unavailable, FlowProvenance::Reset, std::nullopt};
    case OutputKind::CurrentSource:
        if (input.status == Status::Disabled)
            return {FlowOwnerPolicy::ConsumerFallback, FlowProvenance::Unknown, std::nullopt};
        return {input.settingsEnabled ? FlowOwnerPolicy::NrOwned : FlowOwnerPolicy::Unavailable,
            input.resetTemporal ? FlowProvenance::Reset : FlowProvenance::Unknown, std::nullopt};
    }
    return {};
}

struct FlowBroker::Impl {
    struct Slot {
        ComPtr<ID3D12Resource> motion;
        ComPtr<ID3D12Fence> ready, srReturn, fgReturn;
        HANDLE motionHandle = nullptr, readyHandle = nullptr, srHandle = nullptr, fgHandle = nullptr;
        FramePairKey pair{};
        SharedFlowSlot guide{};
        bool published = false, srLeased = false, fgLeased = false;
        uint64_t readyValue = 0, srValue = 0, fgValue = 0;
        Slot() = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        ~Slot() { close_handles(); }
        void close_handles() noexcept
        {
            for (auto* handle : {&motionHandle, &readyHandle, &srHandle, &fgHandle}) {
                if (*handle) CloseHandle(*handle);
                *handle = nullptr;
            }
        }
        void release() noexcept
        {
            close_handles();
            motion.Reset(); ready.Reset(); srReturn.Reset(); fgReturn.Reset();
            published = false;
        }
    };
    struct DisplayRecord {
        uint64_t generation = 0, tick = 0;
        bool used = false;
        DisplayedFlow value;
    };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> source;
    std::array<Slot, flow_ring_slots> slots;
    const Extent extent;
    const uint64_t instance;
    uint32_t descriptorStride = 0, nextSlot = 0;
    std::optional<uint32_t> pending;
    FramePairKey pendingPair{};
    SharedFlowSlot pendingGuide{};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> exported{0}, dropped{0}, reused{0}, displays{0};
    std::array<DisplayRecord, 32> records{};
    uint32_t recordHead = 0;
    mutable std::mutex lock;
    bool poisoned = false, retired = false, closed = false;
    Impl(Extent size, uint64_t id) : extent(size), instance(id) {}
    void initialize(ID3D12Device*, ID3D12CommandQueue*);
    void create_slot(Slot&, uint32_t index);
    bool reusable(const Slot&, uint32_t index) const;
    bool retirable() const;
    void release();
    std::optional<ExportedFlow> lease_locked(uint32_t, FlowConsumer);
};

void FlowBroker::Impl::create_slot(Slot& slot, uint32_t index)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = extent.width;
    description.Height = extent.height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R16G16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    checked(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_SHARED, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.motion)), "flow ring shared texture");
    checked(device->CreateSharedHandle(slot.motion.Get(), nullptr, GENERIC_ALL, nullptr, &slot.motionHandle),
        "flow ring texture NT handle");
    for (auto* fence : {slot.ready.GetAddressOf(), slot.srReturn.GetAddressOf(), slot.fgReturn.GetAddressOf()})
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence)), "flow ring shared fence");
    checked(device->CreateSharedHandle(slot.ready.Get(), nullptr, GENERIC_ALL, nullptr, &slot.readyHandle),
        "flow ring ready NT handle");
    checked(device->CreateSharedHandle(slot.srReturn.Get(), nullptr, GENERIC_ALL, nullptr, &slot.srHandle),
        "flow ring SR return NT handle");
    checked(device->CreateSharedHandle(slot.fgReturn.Get(), nullptr, GENERIC_ALL, nullptr, &slot.fgHandle),
        "flow ring FG return NT handle");
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R16G16_FLOAT;
    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(descriptorStride) * (1u + index);
    device->CreateUnorderedAccessView(slot.motion.Get(), nullptr, &view, cpu);
}

void FlowBroker::Impl::initialize(ID3D12Device* owner, ID3D12CommandQueue* commands)
{
    if (!owner || !commands || !extent.width || !extent.height ||
        extent.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || extent.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::runtime_error("flow broker device/queue/extent admission");
    ComPtr<ID3D12Device> queueDevice;
    checked(commands->GetDevice(IID_PPV_ARGS(&queueDevice)), "flow broker queue device");
    ComPtr<IUnknown> owned, queued;
    checked(owner->QueryInterface(IID_PPV_ARGS(&owned)), "flow broker device identity");
    checked(queueDevice.As(&queued), "flow broker queue identity");
    if (owned.Get() != queued.Get()) throw std::runtime_error("flow broker queue is not on its device");
    device = owner;
    queue = commands;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 8;
    for (UINT i = 0; i < 2; ++i) {
        parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 3;
    signature.pParameters = parameters;
    ComPtr<ID3DBlob> blob, error;
    checked(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
        "flow broker root serialize");
    checked(owner->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&root)), "flow broker root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root.Get();
    state.CS = {nr_flow_export_shader, static_cast<SIZE_T>(sizeof nr_flow_export_shader)};
    checked(owner->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipeline)), "flow broker sanitize pipeline");
    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors = 1 + flow_ring_slots;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    checked(owner->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap)), "flow broker descriptors");
    descriptorStride = owner->GetDescriptorHandleIncrementSize(heapDescription.Type);
    for (uint32_t index = 0; index < slots.size(); ++index) create_slot(slots[index], index);
}

bool FlowBroker::Impl::reusable(const Slot& slot, uint32_t index) const
{
    if (pending && *pending == index) return false;
    if (!slot.published) return true;
    if (!complete(slot.ready.Get(), slot.readyValue)) return false;
    if (slot.srLeased && !complete(slot.srReturn.Get(), slot.srValue)) return false;
    if (slot.fgLeased && !complete(slot.fgReturn.Get(), slot.fgValue)) return false;
    return true;
}
bool FlowBroker::Impl::retirable() const
{
    if (pending) return false;
    for (uint32_t index = 0; index < slots.size(); ++index)
        if (!reusable(slots[index], index)) return false;
    return true;
}
void FlowBroker::Impl::release()
{
    for (auto& slot : slots) slot.release();
    source.Reset(); heap.Reset(); pipeline.Reset(); root.Reset();
    device.Reset(); queue.Reset();
    closed = true;
}
std::optional<ExportedFlow> FlowBroker::Impl::lease_locked(uint32_t index, FlowConsumer consumer)
{
    if (closed || index >= slots.size()) return std::nullopt;
    auto& slot = slots[index];
    if (!slot.published) return std::nullopt;
    ExportedFlow result{slot.pair, slot.guide, slot.motionHandle, slot.readyHandle, nullptr,
        slot.readyValue, 0, index};
    if (consumer == FlowConsumer::Sr) {
        slot.srLeased = true;
        result.returnFence = slot.srHandle;
        result.returnValue = ++slot.srValue;
    } else {
        slot.fgLeased = true;
        result.returnFence = slot.fgHandle;
        result.returnValue = ++slot.fgValue;
    }
    return result;
}

FlowBroker::FlowBroker(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
FlowBroker::~FlowBroker()
{
    unregister_flow_broker(this);
    if (close()) return;
    {
        std::lock_guard<std::mutex> guard(impl_->lock);
        impl_->retired = true;
    }
    std::lock_guard<std::mutex> guard(parked_lock());
    collect_parked();
    for (auto& entry : parked_rings())
        if (!entry) { entry = impl_; return; }
    // Two unresolved generations are already preserved; pin this owner for the
    // process lifetime rather than freeing a ring a consumer may still read.
    (void)new std::shared_ptr<Impl>(impl_);
}
std::array<std::shared_ptr<FlowBroker::Impl>, flow_ring_generation_limit>& FlowBroker::parked_rings()
{
    static std::array<std::shared_ptr<Impl>, flow_ring_generation_limit> value{};
    return value;
}
std::mutex& FlowBroker::parked_lock()
{
    static std::mutex value;
    return value;
}
void FlowBroker::collect_parked()
{
    for (auto& entry : parked_rings()) {
        if (!entry) continue;
        bool done = false;
        {
            std::lock_guard<std::mutex> guard(entry->lock);
            if (entry->retirable()) entry->release();
            done = entry->closed;
        }
        if (done) entry.reset();
    }
}
std::shared_ptr<FlowBroker> FlowBroker::create(const FlowBrokerTarget& target, std::string* diagnostic)
{
    try {
        std::lock_guard<std::mutex> guard(parked_lock());
        collect_parked();
        if (std::all_of(parked_rings().begin(), parked_rings().end(), [](const auto& e) { return e != nullptr; }))
            throw std::runtime_error("flow ring retirement budget: two unresolved generations");
        auto impl = std::make_shared<Impl>(target.analysis, target.sourceInstanceId);
        impl->initialize(target.device, target.queue);
        std::shared_ptr<FlowBroker> broker(new FlowBroker(std::move(impl)));
        register_flow_broker(broker);
        return broker;
    } catch (const std::exception& error) {
        if (diagnostic) *diagnostic = error.what();
        return {};
    }
}
uint64_t FlowBroker::source_instance_id() const { return impl_->instance; }
uint64_t FlowBroker::resource_generation() const { return impl_->generation.load(std::memory_order_acquire); }
uint32_t FlowBroker::slot_count() const { return flow_ring_slots; }
FlowBroker::Stats FlowBroker::stats() const
{
    return {impl_->exported.load(std::memory_order_relaxed), impl_->dropped.load(std::memory_order_relaxed),
        impl_->reused.load(std::memory_order_relaxed), impl_->displays.load(std::memory_order_relaxed)};
}
bool FlowBroker::record_export(ID3D12GraphicsCommandList* list, const FlowExport& exportFlow)
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    if (owner.closed || owner.retired || owner.poisoned || owner.pending || !list || !exportFlow.motion ||
        !exportFlow.previous || exportFlow.guide.reset) return false;
    const FramePairKey pair{{exportFlow.previous->key.frame.instanceId, exportFlow.previous->canonicalTick,
            exportFlow.previous->key.historyEpoch, exportFlow.previous->key.frame.contentEpoch,
            exportFlow.previous->key.submissionSerial, exportFlow.previous->key.resourceGeneration},
        {exportFlow.current.key.frame.instanceId, exportFlow.current.canonicalTick,
            exportFlow.current.key.historyEpoch, exportFlow.current.key.frame.contentEpoch,
            exportFlow.current.key.submissionSerial, exportFlow.current.key.resourceGeneration}};
    if (!pair.valid() || !exportFlow.guide.valid(owner.extent)) return false;
    const auto description = exportFlow.motion->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Format != DXGI_FORMAT_R16G16_FLOAT || description.Width != owner.extent.width ||
        description.Height != owner.extent.height || description.DepthOrArraySize != 1 ||
        description.MipLevels != 1 || description.SampleDesc.Count != 1) return false;
    std::optional<uint32_t> chosen;
    for (uint32_t offset = 0; offset < owner.slots.size(); ++offset) {
        const auto index = (owner.nextSlot + offset) % static_cast<uint32_t>(owner.slots.size());
        if (owner.reusable(owner.slots[index], index)) { chosen = index; break; }
    }
    if (!chosen) {
        owner.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    owner.nextSlot = (*chosen + 1) % static_cast<uint32_t>(owner.slots.size());
    auto& slot = owner.slots[*chosen];
    if (slot.published) owner.reused.fetch_add(1, std::memory_order_relaxed);
    slot.published = false; // the copy overwrites the content; never advertise it while it runs
    slot.pair = {};
    slot.guide = {};
    slot.srLeased = false;
    slot.fgLeased = false;
    if (owner.source.Get() != exportFlow.motion) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16_FLOAT;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        owner.device->CreateShaderResourceView(exportFlow.motion, &view,
            owner.heap->GetCPUDescriptorHandleForHeapStart());
        owner.source = exportFlow.motion;
    }
    transition(list, exportFlow.motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, slot.motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[]{owner.heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetPipelineState(owner.pipeline.Get());
    list->SetComputeRootSignature(owner.root.Get());
    const std::array<uint32_t, 8> parameters{owner.extent.width, owner.extent.height,
        exportFlow.guide.activeX, exportFlow.guide.activeY, exportFlow.guide.activeWidth,
        exportFlow.guide.activeHeight, 0, 0};
    list->SetComputeRoot32BitConstants(0, 8, parameters.data(), 0);
    auto gpu = owner.heap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(1, gpu);
    gpu.ptr += static_cast<SIZE_T>(owner.descriptorStride) * (1u + *chosen);
    list->SetComputeRootDescriptorTable(2, gpu);
    list->Dispatch((owner.extent.width + 7) / 8, (owner.extent.height + 7) / 8, 1);
    transition(list, slot.motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    transition(list, exportFlow.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    owner.pending = *chosen;
    owner.pendingPair = pair;
    owner.pendingGuide = exportFlow.guide;
    return true;
}
void FlowBroker::commit_export(ID3D12CommandQueue* submitted)
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    if (!owner.pending) return;
    const auto index = *owner.pending;
    owner.pending.reset();
    auto& slot = owner.slots[index];
    const auto value = ++slot.readyValue;
    if (!submitted || submitted != owner.queue.Get() || FAILED(owner.queue->Signal(slot.ready.Get(), value))) {
        owner.poisoned = true;
        return;
    }
    slot.pair = owner.pendingPair;
    slot.guide = owner.pendingGuide;
    slot.published = true;
    slot.srLeased = false;
    slot.fgLeased = false;
    owner.generation.store(slot.pair.currColorKey.resourceGeneration, std::memory_order_release);
    owner.exported.fetch_add(1, std::memory_order_relaxed);
}
void FlowBroker::abandon_export()
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    if (!owner.pending) return;
    const auto index = *owner.pending;
    owner.pending.reset();
    auto& slot = owner.slots[index];
    // A submitted-but-refused list may still be executing: fence the slot to its
    // current queue position before any later reuse and never publish it.
    if (FAILED(owner.queue->Signal(slot.ready.Get(), ++slot.readyValue))) owner.poisoned = true;
}
std::optional<FramePairKey> FlowBroker::pair_for_job(uint64_t generation, uint64_t displayTick,
    uint64_t submissionSerial) const
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    for (const auto& slot : owner.slots)
        if (slot.published && slot.pair.valid() &&
            slot.pair.currColorKey.resourceGeneration == generation &&
            slot.pair.currColorKey.submissionSerial == submissionSerial &&
            slot.pair.currColorKey.canonicalTick + 1 == displayTick) return slot.pair;
    return std::nullopt;
}
void FlowBroker::record_display(uint64_t generation, uint64_t displayTick, const DisplayedFlow& value)
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    if (owner.closed) return;
    owner.records[owner.recordHead % owner.records.size()] = {generation, displayTick, true, value};
    owner.recordHead = (owner.recordHead + 1) % static_cast<uint32_t>(owner.records.size());
    owner.displays.fetch_add(1, std::memory_order_relaxed);
}
std::optional<DisplayedFlow> FlowBroker::displayed(uint64_t generation, uint64_t displayTick) const
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    for (const auto& record : owner.records)
        if (record.used && record.generation == generation && record.tick == displayTick) return record.value;
    return std::nullopt;
}
std::optional<ExportedFlow> FlowBroker::lease(uint32_t slot, FlowConsumer consumer)
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    return owner.lease_locked(slot, consumer);
}
std::optional<ExportedFlow> FlowBroker::find_pair(const FramePairKey& pair, FlowConsumer consumer)
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    for (uint32_t index = 0; index < owner.slots.size(); ++index)
        if (owner.slots[index].published && owner.slots[index].pair == pair)
            return owner.lease_locked(index, consumer);
    return std::nullopt;
}
std::optional<ExportedFlow> FlowBroker::find_displayed(uint64_t generation, uint64_t displayTick,
    FlowConsumer consumer)
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    for (const auto& record : owner.records)
        if (record.used && record.generation == generation && record.tick == displayTick && record.value.pair)
            for (uint32_t index = 0; index < owner.slots.size(); ++index)
                if (owner.slots[index].published && owner.slots[index].pair == *record.value.pair)
                    return owner.lease_locked(index, consumer);
    return std::nullopt;
}
bool FlowBroker::close()
{
    auto& owner = *impl_;
    std::lock_guard<std::mutex> guard(owner.lock);
    if (owner.closed) return true;
    if (!owner.retirable()) return false;
    owner.release();
    return true;
}

void register_flow_broker(const std::shared_ptr<FlowBroker>& broker)
{
    if (!broker) return;
    std::lock_guard<std::mutex> guard(registry().lock);
    for (auto& entry : registry().entries)
        if (!entry.broker.lock()) { entry = {broker.get(), broker}; return; }
    registry().entries[0] = {broker.get(), broker}; // four live brokers is already abnormal
}
void unregister_flow_broker(const FlowBroker* broker)
{
    std::lock_guard<std::mutex> guard(registry().lock);
    for (auto& entry : registry().entries)
        if (entry.identity == broker) { entry = {}; return; }
}
std::shared_ptr<FlowBroker> find_flow_broker(uint64_t sourceInstanceId, uint64_t resourceGeneration)
{
    std::lock_guard<std::mutex> guard(registry().lock);
    std::shared_ptr<FlowBroker> unbound;
    for (auto& entry : registry().entries) {
        auto broker = entry.broker.lock();
        if (!broker || broker->source_instance_id() != sourceInstanceId) continue;
        const auto generation = broker->resource_generation();
        if (generation == resourceGeneration) return broker;
        if (!generation && !unbound) unbound = std::move(broker);
    }
    return unbound;
}
} // namespace nr
