// SPDX-License-Identifier: GPL-3.0-only
#include "anr_guides.h"
#include "anr_commands.h"
#include "anr_d3d.h"

namespace nr::fg::vfg {
#ifdef OBS_NR_ANR_MOCK_TEST
HRESULT anr_mock_consumer_reset(const AnrEvaluation&);
#endif
namespace {
constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
constexpr auto copyDest = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr auto copySource = D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto shaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(uint32_t);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
        type == D3D12_HEAP_TYPE_READBACK ? copyDest : common, nullptr, IID_PPV_ARGS(&resource)), "ANR status buffer");
    return resource;
}
} // namespace

AnrCommands::AnrCommands(ID3D12Device* device, ID3D12CommandQueue* queue) : queue_(queue)
{
    constexpr auto direct = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandAllocator(direct, IID_PPV_ARGS(&alloc_)), "ANR allocator");
    for (auto& list : lists_) {
        check(device->CreateCommandList(0, direct, alloc_.Get(), nullptr, IID_PPV_ARGS(&list)), "ANR list");
        check(list->Close(), "ANR initial close");
    }
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "ANR fence");
    event_.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_.h) check(HRESULT_FROM_WIN32(GetLastError()), "ANR fence event");
}

ID3D12GraphicsCommandList* AnrCommands::begin()
{
    auto* list = lists_.at(cursor_++).Get();
    check(list->Reset(alloc_.Get(), nullptr), "ANR list reset");
    return list;
}

NV_OF_FENCE_POINT AnrCommands::submit(ID3D12GraphicsCommandList* list)
{
    execute(list);
    return seal();
}

void AnrCommands::execute(ID3D12GraphicsCommandList* list)
{
    discard(list);
    ID3D12CommandList* lists[]{list};
    untracked_ = true;
    queue_->ExecuteCommandLists(1, lists);
}

void AnrCommands::enqueue_wait(NV_OF_FENCE_POINT point)
{
    check(queue_->Wait(point.fence, point.value), "ANR guide dependency");
    untracked_ = true;
}

NV_OF_FENCE_POINT AnrCommands::seal()
{
    const auto value = last_ + 1;
    check(queue_->Signal(fence_.Get(), value), "ANR fence signal");
    last_ = value;
    untracked_ = false;
    return {fence_.Get(), last_};
}

void AnrCommands::discard(ID3D12GraphicsCommandList* list)
{
    check(list->Close(), "ANR command list close");
}

void AnrCommands::wait_for(NV_OF_FENCE_POINT point)
{
    auto completed = point.fence->GetCompletedValue();
    if (completed == UINT64_MAX) check(DXGI_ERROR_DEVICE_REMOVED, "ANR fence removed");
    if (completed >= point.value) return;
    check(point.fence->SetEventOnCompletion(point.value, event_.h), "ANR fence arm");
    if (WaitForSingleObject(event_.h, 1000) != WAIT_OBJECT_0) check(E_FAIL, "ANR fence wait");
    completed = point.fence->GetCompletedValue();
    if (completed == UINT64_MAX || completed < point.value) check(E_FAIL, "ANR fence incomplete");
}

void AnrCommands::wait()
{
    if (untracked_) (void)seal();
    if (last_) wait_for({fence_.Get(), last_});
}

bool AnrCommands::complete(UINT64 value) const
{
    const auto done = fence_->GetCompletedValue();
    return done != UINT64_MAX && done >= value;
}

void AnrCommands::recycle()
{
    check(alloc_->Reset(), "ANR command allocator reset");
    cursor_ = 0;
}

AnrDispatch::AnrDispatch(ID3D12Device* device, Configuration config)
    : trace_(config.trace), disable_(buffer(device, D3D12_HEAP_TYPE_DEFAULT)), initial_(buffer(device, D3D12_HEAP_TYPE_UPLOAD))
{
    void* data{};
    const D3D12_RANGE read{0, 0}, written{0, sizeof(uint32_t)};
    check(initial_->Map(0, &read, &data), "ANR status upload");
    *static_cast<uint32_t*>(data) = 1u;
    initial_->Unmap(0, &written);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = config.size.width;
    desc.Height = config.size.height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (auto& slot : std::span(slots_).first(config.generated)) {
        slot.readback = buffer(device, D3D12_HEAP_TYPE_READBACK);
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, common, nullptr,
            IID_PPV_ARGS(&slot.image)), "ANR output texture");
    }
}

bool AnrDispatch::evaluate(AnrCommands& commands, AnrFeature& feature, AnrEvaluation dispatch)
{
    auto& output = slots_[dispatch.index - 1];
    dispatch.output = output.image.Get();
    dispatch.disable = disable_.Get();
    auto* producer = commands.begin();
    if (dispatch.index == 1) {
        transition(producer, dispatch.disable, common, copyDest);
        producer->CopyBufferRegion(dispatch.disable, 0, initial_.Get(), 0, sizeof(uint32_t));
        transition(producer, dispatch.disable, copyDest, uav);
    } else {
        transition(producer, dispatch.disable, common, uav);
    }
    transition(producer, dispatch.color, common, shaderRead);
    transition(producer, dispatch.output, common, uav);
    for (auto* texture : {dispatch.motion, dispatch.depth}) transition(producer, texture, common, copyDest);
    if (!feature.evaluate(producer, dispatch)) {
        commands.discard(producer);
        return false;
    }
    for (auto* texture : {dispatch.motion, dispatch.depth}) transition(producer, texture, copyDest, common);
    commands.execute(producer);
    // A fence proves ordering, not that the consumer's transitions exist.
    restorePending_ = true;
    const auto produced = commands.seal().value;
    if (trace_) fprintf(trace_, "producer frame=%llu sub=%u fence=%llu\n", dispatch.frameId, dispatch.index, produced);

#ifdef OBS_NR_ANR_MOCK_TEST
    check(anr_mock_consumer_reset(dispatch), "ANR fixture consumer reset HRESULT");
#endif
    auto* consumer = commands.begin();
    for (auto* resource : {dispatch.output, dispatch.disable}) {
        D3D12_RESOURCE_BARRIER order{};
        order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        order.UAV.pResource = resource;
        consumer->ResourceBarrier(1, &order);
    }
    // Buffers decay to COMMON after ExecuteCommandLists, including explicit UAV use.
    transition(consumer, dispatch.disable, common, copySource);
    consumer->CopyBufferRegion(output.readback.Get(), 0, dispatch.disable, 0, sizeof(uint32_t));
    transition(consumer, dispatch.disable, copySource, common);
    transition(consumer, dispatch.output, uav, common);
    transition(consumer, dispatch.color, shaderRead, common);
    output.fence = commands.submit(consumer).value;
    restorePending_ = false;
    if (trace_) fprintf(trace_, "consumer frame=%llu sub=%u fence=%llu\n", dispatch.frameId, dispatch.index, output.fence);
    return true;
}
} // namespace nr::fg::vfg
