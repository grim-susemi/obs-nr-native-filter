// SPDX-License-Identifier: GPL-3.0-only
#include "xefg_presenter.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace nr::fg::vfg {
namespace {
constexpr float pi = 3.14159265358979323846f;
constexpr uint32_t max_guide_extent = 8192;

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}

void copy_texture(ID3D12GraphicsCommandList* cmd, ID3D12Resource* source, ID3D12Resource* destination)
{
    D3D12_RESOURCE_BARRIER barriers[]{
        transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
    cmd->ResourceBarrier(2, barriers);
    cmd->CopyResource(destination, source);
    for (auto& barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(2, barriers);
}

struct EventHandle {
    HANDLE value{};
    ~EventHandle() { if (value) CloseHandle(value); }
};

bool finite(float value) noexcept
{
    return std::isfinite(value);
}

float sanitized_scale(float value) noexcept
{
    return finite(value) && value != 0.0f ? value : 1.0f;
}

[[nodiscard]] bool succeeded(xefg_swapchain_result_t result) noexcept
{
    return result >= XEFG_SWAPCHAIN_RESULT_SUCCESS;
}
} // namespace

xefg_swapchain_frame_constant_data_t xefg_frame_constants(const XeFgFrameInput& input) noexcept
{
    xefg_swapchain_frame_constant_data_t constants{};
    for (unsigned i = 0; i < 16; ++i) constants.viewMatrix[i] = i % 5 == 0 ? 1.0f : 0.0f;
    const float aspect = input.height ? static_cast<float>(input.width) / static_cast<float>(input.height) : 1.0f;
    const float f = 1.0f / std::tan(60.0f * pi / 360.0f);
    constexpr float nearZ = 0.1f, farZ = 1000.0f;
    const float projection[16]{
        f / aspect, 0.0f, 0.0f, 0.0f,
        0.0f, f, 0.0f, 0.0f,
        0.0f, 0.0f, farZ / (nearZ - farZ), -1.0f,
        0.0f, 0.0f, nearZ * farZ / (nearZ - farZ), 0.0f};
    std::memcpy(constants.projectionMatrix, projection, sizeof(projection));
    constants.jitterOffsetX = 0.0f;
    constants.jitterOffsetY = 0.0f;
    constants.motionVectorScaleX = input.mvScaleX;
    constants.motionVectorScaleY = input.mvScaleY;
    constants.resetHistory = input.shotCut || input.discontinuity || input.flowReset ? 1u : 0u;
    constants.frameRenderTime = std::clamp(input.frameRenderTimeMs, 0.0f, 100.0f);
    return constants;
}

void XeFgGuides::reset() noexcept
{
    // The caller must have drained the queue (or be tearing the generation down);
    // releasing upload/graphics resources while XeFG still reads them is the one
    // lifetime rule this ring cannot enforce on its own.
    depth_.Reset();
    zeroMotion_.Reset();
    motion_[0].Reset();
    motion_[1].Reset();
    depthUpload_.Reset();
    zeroUpload_.Reset();
    width_ = height_ = 0;
    slot_ = 0;
    filled_ = false;
}

ID3D12Resource* XeFgGuides::take_motion_slot() noexcept
{
    auto* result = motion_[slot_].Get();
    slot_ = (slot_ + 1) % 2;
    return result;
}

bool XeFgGuides::ensure(Gpu& gpu, uint32_t width, uint32_t height)
{
    if (!width || !height || width > max_guide_extent || height > max_guide_extent) return false;
    if (depth_ && width_ == width && height_ == height && filled_) return true;
    if (!depth_ || width_ != width || height_ != height) {
        reset();
        ID3D12Device* device = gpu.d3d_device();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&depth_))))
            return false;
        desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&zeroMotion_))))
            return false;
        for (auto& motion : motion_)
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&motion))))
                return false;

        UINT64 depthBytes{};
        UINT64 motionBytes{};
        const auto depthDesc = depth_->GetDesc();
        const auto motionDesc = zeroMotion_->GetDesc();
        device->GetCopyableFootprints(&depthDesc, 0, 1, 0, &depthFp_, nullptr, nullptr, &depthBytes);
        device->GetCopyableFootprints(&motionDesc, 0, 1, 0, &motionFp_, nullptr, nullptr, &motionBytes);
        D3D12_HEAP_PROPERTIES upload{};
        upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Width = depthBytes;
        if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&depthUpload_))))
            return false;
        buffer.Width = motionBytes;
        if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&zeroUpload_))))
            return false;
        void* mapped{};
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(depthUpload_->Map(0, &noRead, &mapped))) return false;
        for (uint32_t y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<float*>(static_cast<uint8_t*>(mapped) +
                size_t{y} * depthFp_.Footprint.RowPitch);
            for (uint32_t x = 0; x < width; ++x) row[x] = 1.0f;
        }
        depthUpload_->Unmap(0, nullptr);
        if (FAILED(zeroUpload_->Map(0, &noRead, &mapped))) return false;
        std::memset(mapped, 0, static_cast<size_t>(motionBytes));
        zeroUpload_->Unmap(0, nullptr);
        width_ = width;
        height_ = height;
        filled_ = false;
    }
    auto* depth = depth_.Get();
    auto* zero = zeroMotion_.Get();
    auto* first = motion_[0].Get();
    auto* second = motion_[1].Get();
    auto& depthFp = depthFp_;
    auto& motionFp = motionFp_;
    const auto fence = gpu.record_draw([&](ID3D12GraphicsCommandList* cmd) {
        const auto copy = [&](ID3D12Resource* target, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                              ID3D12Resource* upload) {
            auto barrier = transition(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION destination{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
            D3D12_TEXTURE_COPY_LOCATION source{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, fp};
            cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            cmd->ResourceBarrier(1, &barrier);
        };
        copy(depth, depthFp, depthUpload_.Get());
        copy(zero, motionFp, zeroUpload_.Get());
        copy(first, motionFp, zeroUpload_.Get());
        copy(second, motionFp, zeroUpload_.Get());
    });
    if (!fence) return false;
    filled_ = true;
    return true;
}

#ifdef NR_FG_TEST_MOCK_XESS
bool XeFgGuides::verify_neutral(ID3D12Device* device, ID3D12CommandQueue* queue, DWORD timeoutMs) const
{
    if (!device || !queue || !depth_ || !width_ || !height_) return false;
    const auto desc = depth_->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES readHeap{};
    readHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        return false;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;
    auto barrier = transition(depth_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, footprint};
    D3D12_TEXTURE_COPY_LOCATION source{depth_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence.Get(), 1))) return false;
    EventHandle event;
    event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event.value || FAILED(fence->SetEventOnCompletion(1, event.value))) return false;
    if (WaitForSingleObject(event.value, timeoutMs) != WAIT_OBJECT_0 || fence->GetCompletedValue() != 1)
        return false;
    void* mapped{};
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)};
    if (FAILED(readback->Map(0, &readRange, &mapped))) return false;
    bool neutral = true;
    for (uint32_t y = 0; y < height_ && neutral; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped) +
            size_t{y} * footprint.Footprint.RowPitch;
        for (uint32_t x = 0; x < width_; ++x) {
            float value = 0.0f;
            std::memcpy(&value, row + size_t{x} * sizeof(float), sizeof(value));
            if (value != 1.0f) { neutral = false; break; }
        }
    }
    const D3D12_RANGE noWrite{0, 0};
    readback->Unmap(0, &noWrite);
    return neutral;
}
#endif

struct XeFgPresenter::Impl {
    Gpu& gpu;
    Session& session;
    FILE* log;
    std::unique_ptr<XeFgRuntime> runtime;
    xefg_swapchain_handle_t swapchain{};
    xell_context_handle_t xell{};
    ComPtr<IDXGISwapChain3> proxy;
    XeFgGuides guides;
    uint32_t presentId{};
    uint64_t pairId{};
    uint64_t presents{};
    uint64_t lastPresentNs{};
    LONG lastLowLatency{-1};
    xefg_swapchain_result_t lastStatus{};
    uint32_t lastFrames{}, lastEnabled{};
    bool statusLogged{}, sleepFailureLogged{}, faulted{}, closed{}, initialized{}, latencyReduction{};
};

XeFgPresenter::XeFgPresenter(Gpu& gpu, Session& session, FILE* log)
    : impl_(new Impl{gpu, session, log})
{}

XeFgPresenter::~XeFgPresenter() { (void)close(); }

unsigned XeFgPresenter::version_tag() const noexcept
{
    if (!impl_->runtime) return 0;
    const auto version = impl_->runtime->swapchain_version();
    return (static_cast<unsigned>(version.major) << 16) | (static_cast<unsigned>(version.minor) << 8) |
        version.patch;
}

bool XeFgPresenter::faulted() const noexcept { return impl_->faulted; }

bool XeFgPresenter::open(std::unique_ptr<XeFgRuntime>&& runtime)
{
    auto& s = *impl_;
    if (s.initialized || !runtime || !runtime->loaded()) return false;
    s.runtime = std::move(runtime);
    auto& a = s.runtime->api();
    auto fail = [&](const char* stage, long long result) {
        if (s.log) fprintf(s.log, "event=xefg_init_failed stage=%s result=%lld\n", stage, result);
        (void)close();
        return false;
    };
    auto xellResult = a.xell_create(s.gpu.d3d_device(), &s.xell);
    if (xellResult != XELL_RESULT_SUCCESS) return fail("xell_create", xellResult);
    xell_sleep_params_t sleep{};
    sleep.bLowLatencyMode = InterlockedCompareExchange(&s.session.lowLatency, 0, 0) ? 1u : 0u;
    s.lastLowLatency = sleep.bLowLatencyMode ? 1 : 0;
    xellResult = a.xell_set_sleep_mode(s.xell, &sleep);
    if (xellResult != XELL_RESULT_SUCCESS) return fail("xell_set_sleep_mode", xellResult);
    auto result = a.d3d12_create_context(s.gpu.d3d_device(), &s.swapchain);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) return fail("d3d12_create_context", result);
    xefg_swapchain_properties_t properties{};
    result = a.get_properties(s.swapchain, &properties);
    if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) return fail("get_properties", result);
    const uint32_t interpolations = std::min<uint32_t>(1, properties.maxSupportedInterpolations);
    if (!interpolations) return fail("max_supported_interpolations", properties.maxSupportedInterpolations);
    // Attempt 0 links XeLL latency reduction. A GPU/driver pair can support XeFG
    // while rejecting XeLL (NVIDIA exposes no XeLL device), and the library
    // refuses to enable frame generation after the link was made. Recreate the
    // XeFG context without ever linking XeLL, then keep the official 2x path.
    bool latency = true;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            s.proxy.Reset();
            (void)a.destroy(s.swapchain);
            s.swapchain = nullptr;
            result = a.d3d12_create_context(s.gpu.d3d_device(), &s.swapchain);
            if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS) return fail("d3d12_create_context_retry", result);
        } else {
            result = a.set_latency_reduction(s.swapchain, s.xell);
            if (s.log) fprintf(s.log, "event=xefg_latency_reduction result=%d\n", static_cast<int>(result));
        }
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = s.session.width;
        desc.Height = s.session.height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH;
        xefg_swapchain_d3d12_init_params_t params{};
        params.pApplicationSwapChain = nullptr;
        params.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;
        params.maxInterpolatedFrames = interpolations;
        params.creationNodeMask = params.visibleNodeMask = 1;
        params.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
        result = a.d3d12_init_from_desc(s.swapchain, s.gpu.window(), &desc, nullptr,
            s.gpu.drawQueue.Get(), s.gpu.dxgi_factory(), &params);
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            return fail(attempt ? "d3d12_init_from_swapchain_desc_retry" : "d3d12_init_from_swapchain_desc",
                result);
        result = a.d3d12_get_swapchain_ptr(s.swapchain, IID_PPV_ARGS(&s.proxy));
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            return fail(attempt ? "d3d12_get_swapchain_ptr_retry" : "d3d12_get_swapchain_ptr", result);
        result = a.set_enabled(s.swapchain, 1);
        if (result == XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED && attempt == 0) {
            if (s.log) fprintf(s.log, "event=xefg_latency_reduction_unsupported fallback=disabled\n");
            latency = false;
            continue;
        }
        if (result != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            return fail(attempt ? "set_enabled_retry" : "set_enabled", result);
        break;
    }
    s.latencyReduction = latency;
    s.initialized = true;
    if (s.log) {
        const auto version = s.runtime->swapchain_version();
        fprintf(s.log,
            "event=xefg_initialized version=%u.%u.%u max_interpolations=%u max_supported=%u ui_mode=%u "
            "render=%ux%u draws=%p latency_reduction=%d\n",
            version.major, version.minor, version.patch, interpolations,
            properties.maxSupportedInterpolations, static_cast<unsigned>(XEFG_SWAPCHAIN_UI_MODE_NONE),
            s.session.width, s.session.height, static_cast<void*>(s.proxy.Get()),
            s.latencyReduction ? 1 : 0);
    }
    return true;
}

bool XeFgPresenter::close() noexcept
{
    auto& s = *impl_;
    if (s.closed && !s.runtime && !s.swapchain && !s.xell) return true;
    xefg_swapchain_result_t destroyed = XEFG_SWAPCHAIN_RESULT_SUCCESS;
    xell_result_t xellDestroyed = XELL_RESULT_SUCCESS;
    s.proxy.Reset();
    if (s.swapchain && s.runtime) {
        destroyed = s.runtime->api().destroy(s.swapchain);
        s.swapchain = nullptr;
    }
    if (s.xell && s.runtime) {
        xellDestroyed = s.runtime->api().xell_destroy(s.xell);
        s.xell = nullptr;
    }
    s.guides.reset();
    if (s.runtime) {
        s.runtime->close();
        s.runtime.reset();
    }
    s.initialized = false;
    s.closed = true;
    const bool ok = destroyed == XEFG_SWAPCHAIN_RESULT_SUCCESS && xellDestroyed == XELL_RESULT_SUCCESS;
    if (s.log) fprintf(s.log,
        "event=xefg_teardown destroy_result=%d xell_destroy_result=%d ok=%d presents=%llu\n",
        static_cast<int>(destroyed), static_cast<int>(xellDestroyed), ok ? 1 : 0,
        static_cast<unsigned long long>(s.presents));
    return ok;
}

std::optional<FgPairTicket> XeFgPresenter::submit_pair(const FgPairDesc& pair, unsigned prevIndex,
    unsigned currIndex)
{
    auto& s = *impl_;
    if (!s.initialized) return {};
    // No backend subframes: the next original Present is what makes XeFG emit
    // the interpolated frame, so the pair only carries the tick identity.
    return FgPairTicket{++s.pairId, prevIndex, currIndex,
        std::min(pair.effectiveMultiplier, 2u), 0, 0};
}

bool XeFgPresenter::subframe_ready(const FgPairTicket&, uint32_t sub)
{
    // lastSubmittedSub == 0: the pair is retired by the original Present.
    return sub == 0;
}

void XeFgPresenter::resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource*, uint32_t)
{}

bool XeFgPresenter::tag_resource(ID3D12GraphicsCommandList* cmd, uint32_t presentId,
    const XeFgTagRequest& tag)
{
    auto& s = *impl_;
    xefg_swapchain_d3d12_resource_data_t data{};
    data.type = tag.type;
    data.validity = tag.validity;
    data.resourceBase = {0, 0};
    data.resourceSize = {tag.width, tag.height};
    data.pResource = tag.resource;
    data.incomingState = tag.state;
    const auto result = s.runtime->api().d3d12_tag_resource(s.swapchain, cmd, presentId, &data);
    if (!succeeded(result) && s.log)
        fprintf(s.log, "event=xefg_tag_failed resource=%s present_id=%u result=%d\n",
            tag.name, presentId, static_cast<int>(result));
    return succeeded(result);
}

bool XeFgPresenter::drain_draw_queue(DWORD timeoutMs)
{
    auto& s = *impl_;
    ComPtr<ID3D12Fence> fence;
    EventHandle event;
    if (FAILED(s.gpu.d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event.value) return false;
    if (FAILED(s.gpu.drawQueue->Signal(fence.Get(), 1)) ||
        FAILED(fence->SetEventOnCompletion(1, event.value))) return false;
    if (WaitForSingleObject(event.value, timeoutMs) != WAIT_OBJECT_0 || fence->GetCompletedValue() != 1) {
        if (s.log) fprintf(s.log, "event=xefg_drain_timeout timeout_ms=%lu\n", timeoutMs);
        return false;
    }
    return true;
}

void XeFgPresenter::log_present_status(uint32_t presentId)
{
    auto& s = *impl_;
    xefg_swapchain_present_status_t status{};
    if (s.runtime->api().get_last_present_status(s.swapchain, &status) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return;
    if (s.statusLogged && status.frameGenResult == s.lastStatus && status.framesPresented == s.lastFrames &&
        status.isFrameGenEnabled == s.lastEnabled)
        return;
    s.statusLogged = true;
    s.lastStatus = status.frameGenResult;
    s.lastFrames = status.framesPresented;
    s.lastEnabled = status.isFrameGenEnabled;
    if (s.log) fprintf(s.log,
        "event=xefg_present_status present_id=%u frames=%u result=%d enabled=%u\n", presentId,
        status.framesPresented, static_cast<int>(status.frameGenResult), status.isFrameGenEnabled);
}

std::optional<UINT> XeFgPresenter::present_original(const FgOriginalDesc& original)
{
    auto& s = *impl_;
    if (!s.initialized || !s.proxy || !s.swapchain) return std::nullopt;
    const auto& flow = original.flow;
    const bool shared = flow.owner == ipc::FlowOwner::NrOwned &&
        flow.provenance == ipc::FlowProvenance::Valid && flow.motion && flow.guideWidth && flow.guideHeight;
    const uint32_t guideWidth = shared ? flow.guideWidth : s.session.width;
    const uint32_t guideHeight = shared ? flow.guideHeight : s.session.height;
    if (s.guides.width() && (s.guides.width() != guideWidth || s.guides.height() != guideHeight) &&
        !drain_draw_queue(2000))
        throw ipc::Error(ipc::Status::ProtocolError, E_FAIL, "XeFG guide extent drain");
    if (!s.guides.ensure(s.gpu, guideWidth, guideHeight)) return std::nullopt;
    auto* depth = s.guides.depth();
    auto* motion = s.guides.take_motion_slot();
    auto* zero = s.guides.zero_motion();
    if (!depth || !motion || !zero) return std::nullopt;
    const auto now = clock_ns();
    const float renderTimeMs = s.lastPresentNs && now > s.lastPresentNs
        ? static_cast<float>(static_cast<double>(now - s.lastPresentNs) / 1e6) : 0.0f;
    s.lastPresentNs = now;
    const bool reset = original.discontinuity || original.shotCut || original.repeat || flow.shotCut ||
        flow.reset || !shared;
    const XeFgFrameInput input{original.shotCut || flow.shotCut,
        original.discontinuity || original.repeat, flow.reset || !shared,
        sanitized_scale(flow.mvScaleX), sanitized_scale(flow.mvScaleY), renderTimeMs,
        s.session.width, s.session.height};
    const auto constants = xefg_frame_constants(input);
    uint32_t presentId = ++s.presentId;
    if (!presentId) presentId = s.presentId = 1;
    ComPtr<ID3D12Resource> backbuffer;
    if (FAILED(s.proxy->GetBuffer(s.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
        if (s.log) fprintf(s.log, "event=xefg_get_buffer_failed present_id=%u\n", presentId);
        s.faulted = true;
        throw ipc::Error(ipc::Status::ProtocolError, E_FAIL, "XeFG proxy backbuffer");
    }
    const char* failedCall{};
    xefg_swapchain_result_t failedResult = XEFG_SWAPCHAIN_RESULT_SUCCESS;
    auto* runtime = s.runtime.get();
    const bool presentShared = shared;
    const auto fence = s.gpu.record_draw([&](ID3D12GraphicsCommandList* cmd) {
        if (presentShared) copy_texture(cmd, flow.motion, motion);
        else copy_texture(cmd, zero, motion);
        if (!tag_resource(cmd, presentId, {depth, XEFG_SWAPCHAIN_RES_DEPTH,
                XEFG_SWAPCHAIN_RV_ONLY_NOW, D3D12_RESOURCE_STATE_COMMON, guideWidth, guideHeight,
                "depth"})) {
            failedCall = "TagFrameResourceDepth";
        }
        if (!tag_resource(cmd, presentId, {motion, XEFG_SWAPCHAIN_RES_MOTION_VECTOR,
                XEFG_SWAPCHAIN_RV_ONLY_NOW, D3D12_RESOURCE_STATE_COMMON, guideWidth, guideHeight,
                "motion"})) {
            failedCall = "TagFrameResourceMotion";
        }
        if (!tag_resource(cmd, presentId, {backbuffer.Get(), XEFG_SWAPCHAIN_RES_BACKBUFFER,
                XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT, D3D12_RESOURCE_STATE_PRESENT, s.session.width,
                s.session.height, "backbuffer"})) {
            failedCall = "TagFrameResourceBackbuffer";
        }
        const auto constantsResult = runtime->api().tag_frame_constants(s.swapchain, presentId, &constants);
        if (!succeeded(constantsResult) && !failedCall) {
            failedCall = "TagFrameConstants";
            failedResult = constantsResult;
        }
        const auto idResult = runtime->api().set_present_id(s.swapchain, presentId);
        if (!succeeded(idResult) && !failedCall) {
            failedCall = "SetPresentId";
            failedResult = idResult;
        }
        s.gpu.record_present_copy(cmd, {original.color, backbuffer.Get(), -1, 50, false});
    });
    if (!fence) return std::nullopt;
    s.gpu.mark_inputs_read(original.index, original.index, fence);
    if (failedCall) {
        s.faulted = true;
        if (s.log) fprintf(s.log, "event=xefg_call_failed call=%s present_id=%u result=%d\n",
            failedCall, presentId, static_cast<int>(failedResult));
        throw ipc::Error(ipc::Status::ProtocolError, E_FAIL, failedCall);
    }
    auto& a = s.runtime->api();
    const LONG lowLatency = InterlockedCompareExchange(&s.session.lowLatency, 0, 0) ? 1 : 0;
    if (lowLatency != s.lastLowLatency) {
        xell_sleep_params_t sleep{};
        sleep.bLowLatencyMode = lowLatency ? 1u : 0u;
        const auto result = a.xell_set_sleep_mode(s.xell, &sleep);
        if (s.log) fprintf(s.log, "event=xefg_low_latency enabled=%ld result=%d\n", lowLatency,
            static_cast<int>(result));
        s.lastLowLatency = lowLatency;
    }
    (void)a.xell_add_marker(s.xell, presentId, XELL_SIMULATION_START);
    (void)a.xell_add_marker(s.xell, presentId, XELL_SIMULATION_END);
    (void)a.xell_add_marker(s.xell, presentId, XELL_RENDERSUBMIT_START);
    (void)a.xell_add_marker(s.xell, presentId, XELL_RENDERSUBMIT_END);
    (void)a.xell_add_marker(s.xell, presentId, XELL_PRESENT_START);
    if (lowLatency && s.latencyReduction) {
        const auto result = a.xell_sleep(s.xell, presentId);
        if (result != XELL_RESULT_SUCCESS && !s.sleepFailureLogged && s.log) {
            s.sleepFailureLogged = true;
            fprintf(s.log, "event=xefg_sleep_failed result=%d\n", static_cast<int>(result));
        }
    }
    const HRESULT present = s.proxy->Present(0, 0);
    (void)a.xell_add_marker(s.xell, presentId, XELL_PRESENT_END);
    if (FAILED(present)) {
        s.faulted = true;
        if (s.log) fprintf(s.log, "event=xefg_present_failed present_id=%u hr=0x%08lX\n", presentId,
            static_cast<unsigned long>(present));
        throw ipc::Error(ipc::Status::ProtocolError, present, "XeFG proxy Present");
    }
    ++s.presents;
    if (s.log) fprintf(s.log,
        "event=xefg_frame present_id=%u reset=%d shot_cut=%d discontinuity=%d repeat=%d shared_mv=%d "
        "guide=%ux%u mv_scale=%.4f,%.4f render=%ux%u fence=%llu\n",
        presentId, reset ? 1 : 0, original.shotCut || flow.shotCut ? 1 : 0, original.discontinuity ? 1 : 0,
        original.repeat ? 1 : 0, shared ? 1 : 0, guideWidth, guideHeight,
        input.mvScaleX, input.mvScaleY, s.session.width, s.session.height,
        static_cast<unsigned long long>(fence));
    log_present_status(presentId);
    UINT count{};
    (void)s.proxy->GetLastPresentCount(&count);
    return count ? count : static_cast<UINT>(s.presents);
}
} // namespace nr::fg::vfg
