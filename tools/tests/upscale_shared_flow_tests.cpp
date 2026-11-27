// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - one QA executable owns the SR Shared-NVOF policy contract and
// the real D3D12->D3D11 import lane (shared NT texture/fences, nearest resample,
// sign/scale, NaN sanitation) plus the worker wait-discipline source assertion.
#include <windows.h>
#include "obs_upscale_dlss.h"
#include "obs_upscale_dlss_shader.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
void require(bool value, const char* why)
{
    ++checks;
    if (!value)
        throw std::runtime_error(why);
}
void checked(HRESULT hr, const char* why)
{
    if (FAILED(hr)) {
        char message[256];
        std::snprintf(message, sizeof message, "%s (hr=0x%08lx)", why, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}
float half_to_float(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0)
            bits = sign;
        else {
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400u)) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ffu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float value = 0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}
bool nan_half(uint16_t value)
{
    return (value & 0x7c00u) == 0x7c00u && (value & 0x3ffu) != 0;
}
bool close(float a, float b, float tolerance = 0.01f)
{
    return std::fabs(a - b) <= tolerance;
}
uint16_t to_half(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu)
        return static_cast<uint16_t>(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    const auto adjusted = static_cast<int32_t>(exponent) - 112;
    if (adjusted >= 0x1f)
        return static_cast<uint16_t>(sign | 0x7c00u);
    if (adjusted <= 0) {
        if (adjusted < -10)
            return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        return static_cast<uint16_t>(sign | (mantissa >> static_cast<uint32_t>(14 - adjusted)));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(adjusted) << 10) | (mantissa >> 13));
}

// ---- policy -----------------------------------------------------------------
void policy()
{
    const auto mode = [](bool present, bool enabled, bool ready, bool display, nr::FlowOwnerPolicy owner,
                         nr::FlowProvenance provenance, bool pair) {
        SrFlowInput in;
        in.nrPresent = present;
        in.nrEnabled = enabled;
        in.nrReady = ready;
        in.displayFound = display;
        in.owner = owner;
        in.provenance = provenance;
        in.pairAvailable = pair;
        return sr_flow_mode(in);
    };
    const auto owned = nr::FlowOwnerPolicy::NrOwned;
    const auto fallback = nr::FlowOwnerPolicy::ConsumerFallback;
    require(mode(false, false, false, false, {}, {}, false) == SrFlowMode::LocalFallback,
        "NR absent must use the consumer-local OFA");
    require(mode(true, false, true, true, owned, nr::FlowProvenance::Valid, true) == SrFlowMode::LocalFallback,
        "disabled NR must fall back locally");
    require(mode(true, true, false, true, owned, nr::FlowProvenance::Valid, true) == SrFlowMode::LocalFallback,
        "bypassed NR must fall back locally");
    require(mode(true, true, true, false, owned, nr::FlowProvenance::Valid, true) == SrFlowMode::ForbidLocal,
        "NR-owned chain without displayed provenance must not run local OFA");
    require(mode(true, true, true, true, owned, nr::FlowProvenance::Valid, true) == SrFlowMode::UseShared,
        "NrOwned+Valid consumes the shared pair");
    require(mode(true, true, true, true, owned, nr::FlowProvenance::Valid, false) == SrFlowMode::ForbidLocal,
        "Valid without a leased pair must forbid local OFA");
    require(mode(true, true, true, true, owned, nr::FlowProvenance::HeldPrevious, false) == SrFlowMode::ForbidLocal,
        "HeldPrevious must forbid local OFA");
    require(mode(true, true, true, true, owned, nr::FlowProvenance::DedupReused, false) == SrFlowMode::ForbidLocal,
        "DedupReused must forbid local OFA");
    require(mode(true, true, true, true, owned, nr::FlowProvenance::Reset, false) == SrFlowMode::ForbidLocal,
        "Reset must forbid local OFA");
    require(mode(true, true, true, true, owned, nr::FlowProvenance::Discontinuity, false) == SrFlowMode::ForbidLocal,
        "Discontinuity must forbid local OFA");
    require(mode(true, true, true, true, fallback, nr::FlowProvenance::Unknown, false) == SrFlowMode::LocalFallback,
        "ConsumerFallback must run the local OFA");
    require(mode(true, true, true, true, nr::FlowOwnerPolicy::Unavailable, nr::FlowProvenance::Unknown, false) ==
            SrFlowMode::ForbidLocal, "Unavailable on an NR-owned chain must not run local OFA");

    SrFlowCounters counters;
    for (int i = 0; i < 12; ++i)
        counters.observe(SrFlowMode::UseShared, false);
    require(counters.frames == 12 && counters.sharedHits == 12 && counters.ofaExec == 0 && counters.localFallback == 0,
        "shared hits never execute OFA");
    for (int i = 0; i < 12; ++i)
        counters.observe(SrFlowMode::ForbidLocal, false);
    require(counters.sharedFallbacks == 12 && counters.ofaExec == 0, "forbidden frames never execute OFA");
    for (int i = 0; i < 10; ++i)
        counters.observe(SrFlowMode::LocalFallback, true);
    require(counters.localFallback == 10 && counters.ofaExec == 10,
        "consumer fallback executes the local OFA");
    std::printf("SR counters: frames=%llu ofaExec=%llu sharedHits=%llu sharedFallbacks=%llu localFallback=%llu\n",
        static_cast<unsigned long long>(counters.frames), static_cast<unsigned long long>(counters.ofaExec),
        static_cast<unsigned long long>(counters.sharedHits),
        static_cast<unsigned long long>(counters.sharedFallbacks),
        static_cast<unsigned long long>(counters.localFallback));
}

// ---- guide -> output scale ---------------------------------------------------
void scale()
{
    const auto native = sr_motion_scale(2560, 1440, 1.0f, 1.0f, 2560, 1440, 2560, 1440);
    require(close(native.x, 1.0f) && close(native.y, 1.0f), "native 1440p is the identity scale");
    const auto bars = sr_motion_scale(1344, 756, 1.0f, 1.0f, 1920, 1080, 2560, 1440);
    require(close(bars.x, 2560.0f / 1344.0f) && close(bars.y, 1440.0f / 756.0f),
        "70% workScale guide converts guide pixels to output pixels");
    // +8 input px/frame: the 0.5-frame midpoint is +4 input px, i.e. +2.8 guide px
    // at 70%, i.e. +5.333 output px at 1440p (matches the plan's MV oracle).
    const float guideHalf = 4.0f * 1344.0f / 1920.0f;
    require(close(guideHalf, 2.8f, 0.001f), "fixture premise: 4 input px is 2.8 guide px");
    require(close(guideHalf * bars.x, 5.333333f), "1080p->1440p midpoint is +5.33 output px");
    require(close(-guideHalf * bars.x, -5.333333f), "sign is preserved exactly once");
    require(close(guideHalf * bars.x * (1920.0f / 2560.0f), 4.0f), "NGX inverse scale returns input pixels");
    const auto doubled = sr_motion_scale(1344, 756, 2.0f, 0.5f, 1920, 1080, 2560, 1440);
    require(close(doubled.x, 2.0f * bars.x) && close(doubled.y, 0.5f * bars.y),
        "mvScale is applied exactly once");
    const auto invalid = sr_motion_scale(0, 0, 1.0f, 1.0f, 0, 0, 0, 0);
    require(close(invalid.x, 1.0f) && close(invalid.y, 1.0f), "missing extents keep a safe identity scale");
}

// ---- real D3D12 -> D3D11 shared import ---------------------------------------
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
    ~Gpu()
    {
        if (event)
            CloseHandle(event);
    }
    void init(IDXGIAdapter1* adapter)
    {
        checked(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12 device");
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        checked(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)), "D3D12 queue");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
            "D3D12 allocator");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list)), "D3D12 list");
        checked(list->Close(), "D3D12 initial close");
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "D3D12 fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "D3D12 event");
    }
    void begin()
    {
        checked(allocator->Reset(), "D3D12 allocator reset");
        checked(list->Reset(allocator.Get(), nullptr), "D3D12 list reset");
    }
    void flush()
    {
        checked(list->Close(), "D3D12 list close");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
    }
    void sync()
    {
        const auto next = ++serial;
        checked(fence->SetEventOnCompletion(next, event), "D3D12 completion subscribe");
        checked(queue->Signal(fence.Get(), next), "D3D12 completion signal");
        require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "D3D12 completion timeout");
        require(fence->GetCompletedValue() >= next, "D3D12 device removed");
    }
    void wait_shared(ID3D12Fence* shared, uint64_t value)
    {
        const auto next = ++serial;
        checked(fence->SetEventOnCompletion(next, event), "D3D12 shared wait subscribe");
        checked(queue->Wait(shared, value), "D3D12 shared wait");
        checked(queue->Signal(fence.Get(), next), "D3D12 shared wait order");
        require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "D3D12 shared wait timeout");
    }
};

struct D3d11 {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    void init(IDXGIAdapter1* adapter)
    {
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL chosen{};
        checked(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 2, D3D11_SDK_VERSION, &device, &chosen, &context), "D3D11 device");
        checked(device.As(&device5), "D3D11.4 device");
        checked(context.As(&context4), "D3D11.4 context");
    }
};

ComPtr<ID3D12Resource> shared_motion(Gpu& gpu, uint32_t width, uint32_t height, float mvx, float mvy,
                                     uint32_t nanX, uint32_t nanY, uint32_t nanWidth, uint32_t nanHeight,
                                     HANDLE* shared)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R16G16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    ComPtr<ID3D12Resource> texture;
    checked(gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_SHARED, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)), "shared motion texture");
    checked(gpu.device->CreateSharedHandle(texture.Get(), nullptr, GENERIC_ALL, nullptr, shared),
        "shared motion handle");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    gpu.device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES uploadProperties{};
    uploadProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC uploadDescription{};
    uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDescription.Width = bytes;
    uploadDescription.Height = 1;
    uploadDescription.DepthOrArraySize = 1;
    uploadDescription.MipLevels = 1;
    uploadDescription.SampleDesc.Count = 1;
    uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    checked(gpu.device->CreateCommittedResource(&uploadProperties, D3D12_HEAP_FLAG_NONE, &uploadDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "motion upload");
    void* mapped = nullptr;
    const D3D12_RANGE empty{0, 0};
    checked(upload->Map(0, &empty, &mapped), "motion upload map");
    auto* base = static_cast<uint8_t*>(mapped) + footprint.Offset;
    for (uint32_t y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(base + static_cast<size_t>(y) * footprint.Footprint.RowPitch);
        for (uint32_t x = 0; x < width; ++x) {
            const bool nan = x >= nanX && y >= nanY && x < nanX + nanWidth && y < nanY + nanHeight;
            row[x * 2] = nan ? 0x7fc0u : to_half(mvx);
            row[x * 2 + 1] = nan ? 0x7fc0u : to_half(mvy);
        }
    }
    upload->Unmap(0, nullptr);
    gpu.begin();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    gpu.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = upload.Get();
    origin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    origin.PlacedFootprint = footprint;
    gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    gpu.list->ResourceBarrier(1, &barrier);
    gpu.flush();
    gpu.sync();
    return texture;
}

struct SharedFence {
    ComPtr<ID3D12Fence> fence;
    HANDLE handle = nullptr;
};
SharedFence shared_fence(Gpu& gpu)
{
    SharedFence result;
    checked(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&result.fence)), "shared fence");
    checked(gpu.device->CreateSharedHandle(result.fence.Get(), nullptr, GENERIC_ALL, nullptr, &result.handle),
        "shared fence handle");
    return result;
}

void import_case(Gpu& gpu, D3d11& d3d11, uint32_t guideWidth, uint32_t guideHeight,
                 uint32_t inputWidth, uint32_t inputHeight, uint32_t outputWidth, uint32_t outputHeight,
                 float mvx, float mvy)
{
    HANDLE textureHandle = nullptr;
    auto texture = shared_motion(gpu, guideWidth, guideHeight, mvx, mvy, 6, 6, 4, 4, &textureHandle);
    auto ready = shared_fence(gpu);
    auto back = shared_fence(gpu);
    gpu.queue->Signal(ready.fence.Get(), 1);
    gpu.sync();

    ComPtr<ID3D11Texture2D> shared;
    ComPtr<ID3D11ShaderResourceView> sharedView;
    ComPtr<ID3D11Fence> readyFence, backFence;
    checked(d3d11.device5->OpenSharedResource1(textureHandle, IID_PPV_ARGS(&shared)), "D3D11 open shared motion");
    checked(d3d11.device->CreateShaderResourceView(shared.Get(), nullptr, &sharedView), "D3D11 shared view");
    checked(d3d11.device5->OpenSharedFence(ready.handle, IID_PPV_ARGS(&readyFence)), "D3D11 ready fence");
    checked(d3d11.device5->OpenSharedFence(back.handle, IID_PPV_ARGS(&backFence)), "D3D11 return fence");

    D3D11_TEXTURE2D_DESC outputDescription{};
    outputDescription.Width = inputWidth;
    outputDescription.Height = inputHeight;
    outputDescription.MipLevels = 1;
    outputDescription.ArraySize = 1;
    outputDescription.Format = DXGI_FORMAT_R16G16_FLOAT;
    outputDescription.SampleDesc.Count = 1;
    outputDescription.Usage = D3D11_USAGE_DEFAULT;
    outputDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    checked(d3d11.device->CreateTexture2D(&outputDescription, nullptr, &output), "D3D11 motion output");
    checked(d3d11.device->CreateUnorderedAccessView(output.Get(), nullptr, &outputView), "D3D11 motion UAV");

    const auto code = [&] {
        ComPtr<ID3DBlob> blob, error;
        const auto hr = D3DCompile(motionImportShader, sizeof(motionImportShader) - 1, "import_shared", nullptr,
            nullptr, "import_shared", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
            0, &blob, &error);
        if (FAILED(hr) && error)
            std::printf("import compile error: %s\n", static_cast<const char*>(error->GetBufferPointer()));
        checked(hr, "import shader compile");
        return blob;
    }();
    ComPtr<ID3D11ComputeShader> importShader;
    checked(d3d11.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &importShader),
        "import shader");

    const auto scale = sr_motion_scale(guideWidth, guideHeight, 1.0f, 1.0f, inputWidth, inputHeight,
                                       outputWidth, outputHeight);
    const float constants[]{static_cast<float>(guideWidth), static_cast<float>(guideHeight), scale.x, scale.y};
    D3D11_BUFFER_DESC constantsDescription{};
    constantsDescription.ByteWidth = sizeof constants;
    constantsDescription.Usage = D3D11_USAGE_DEFAULT;
    constantsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA constantsData{constants, 0, 0};
    ComPtr<ID3D11Buffer> constantBuffer;
    checked(d3d11.device->CreateBuffer(&constantsDescription, &constantsData, &constantBuffer), "import constants");

    d3d11.context4->Wait(readyFence.Get(), 1);
    ID3D11ShaderResourceView* srvs[]{sharedView.Get()};
    ID3D11UnorderedAccessView* uavs[]{outputView.Get()};
    ID3D11Buffer* cb = constantBuffer.Get();
    ID3D11ShaderResourceView* noSrv[1]{};
    ID3D11UnorderedAccessView* noUav[1]{};
    d3d11.context->CSSetShader(importShader.Get(), nullptr, 0);
    d3d11.context->CSSetShaderResources(0, 1, srvs);
    d3d11.context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    d3d11.context->CSSetConstantBuffers(0, 1, &cb);
    d3d11.context->Dispatch((inputWidth + 7) / 8, (inputHeight + 7) / 8, 1);
    d3d11.context->CSSetShaderResources(0, 1, noSrv);
    d3d11.context->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
    checked(d3d11.context4->Signal(backFence.Get(), 7), "D3D11 SR return signal");
    d3d11.context->Flush();
    gpu.wait_shared(back.fence.Get(), 7); // proves the D3D11 signal reached the shared D3D12 fence

    D3D11_TEXTURE2D_DESC stagingDescription = outputDescription;
    stagingDescription.BindFlags = 0;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    checked(d3d11.device->CreateTexture2D(&stagingDescription, nullptr, &staging), "motion staging");
    d3d11.context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(d3d11.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "motion staging map");
    uint64_t compared = 0, mismatched = 0, nans = 0, sanitized = 0;
    for (uint32_t y = 0; y < inputHeight; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (uint32_t x = 0; x < inputWidth; ++x) {
            uint16_t value[2]{};
            std::memcpy(value, row + static_cast<size_t>(x) * 4, 4);
            if (nan_half(value[0]) || nan_half(value[1])) {
                ++nans;
                continue;
            }
            const int gx = std::min<int>(static_cast<int>((static_cast<float>(x) + 0.5f) /
                static_cast<float>(inputWidth) * static_cast<float>(guideWidth)), static_cast<int>(guideWidth) - 1);
            const int gy = std::min<int>(static_cast<int>((static_cast<float>(y) + 0.5f) /
                static_cast<float>(inputHeight) * static_cast<float>(guideHeight)), static_cast<int>(guideHeight) - 1);
            const bool nanSource = gx >= 6 && gy >= 6 && gx < 10 && gy < 10;
            const float expectedX = nanSource ? 0.0f : mvx * scale.x;
            const float expectedY = nanSource ? 0.0f : mvy * scale.y;
            const float actualX = half_to_float(value[0]), actualY = half_to_float(value[1]);
            if (nanSource && actualX == 0.0f && actualY == 0.0f)
                ++sanitized;
            if (!close(actualX, expectedX) || !close(actualY, expectedY)) {
                if (mismatched < 5)
                    std::printf("IMPORT MISMATCH x=%u y=%u got=(%.4f,%.4f) expected=(%.4f,%.4f)\n", x, y,
                        actualX, actualY, expectedX, expectedY);
                ++mismatched;
            }
            ++compared;
        }
    }
    d3d11.context->Unmap(staging.Get(), 0);
    std::printf("IMPORT %ux%u guide -> %ux%u (out %ux%u): compared=%llu mismatch=%llu nan=%llu sanitized=%llu scale=(%.4f,%.4f)\n",
        guideWidth, guideHeight, inputWidth, inputHeight, outputWidth, outputHeight,
        static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatched),
        static_cast<unsigned long long>(nans), static_cast<unsigned long long>(sanitized), scale.x, scale.y);
    require(nans == 0 && mismatched == 0, "imported SR motion must be finite and exactly scaled");
    require(sanitized > 0, "NaN guide texels must sanitize to zero");
    (void)texture; // kept alive until every consumer signal completed
}

void gpu_import()
{
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::printf("SKIP shared import: no DXGI factory\n");
        return;
    }
    ComPtr<IDXGIAdapter1> chosen;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        ComPtr<ID3D12Device> probe;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&probe))))
            continue;
        chosen = adapter;
        if (description.VendorId == 0x10de)
            break;
    }
    if (!chosen) {
        std::printf("SKIP shared import: no hardware D3D12 adapter\n");
        return;
    }
    Gpu gpu;
    D3d11 d3d11;
    gpu.init(chosen.Get());
    d3d11.init(chosen.Get());
    // 1080p -> 1440p with a 70% workScale guide, then the native 1:1 arm.
    import_case(gpu, d3d11, 1344, 756, 1920, 1080, 2560, 1440, 2.8f, -1.5f);
    import_case(gpu, d3d11, 320, 180, 320, 180, 320, 180, 4.0f, 2.0f);
}

// ---- worker wait discipline --------------------------------------------------
std::string read_source()
{
#ifdef UPSCALE_DLSS_SOURCE
    std::ifstream file(UPSCALE_DLSS_SOURCE, std::ios::binary);
    require(static_cast<bool>(file), "obs_upscale_dlss.cpp must be readable for the wait-discipline assertion");
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
#else
    require(false, "UPSCALE_DLSS_SOURCE is not defined");
    return {};
#endif
}
std::string function_body(const std::string& source, const std::string& signature)
{
    const auto start = source.find(signature);
    require(start != std::string::npos, "Session::run signature missing");
    const auto open = source.find('{', start);
    require(open != std::string::npos, "Session::run body missing");
    int depth = 0;
    for (auto at = open; at < source.size(); ++at) {
        if (source[at] == '{') ++depth;
        else if (source[at] == '}' && --depth == 0)
            return source.substr(open, at - open + 1);
    }
    require(false, "Session::run body is unbalanced");
    return {};
}
void mutex_discipline()
{
    const auto body = function_body(read_source(), "bool run(const Capture& frame)");
    require(body.find("std::unique_lock lock(ngxMutex)") != std::string::npos,
        "Session::run must hold the NGX context through a releasable lock");
    require(body.find("std::lock_guard lock(ngxMutex)") == std::string::npos,
        "Session::run must not pin ngxMutex across the whole frame");
    unsigned waits = 0;
    for (auto at = body.find("finish_worker()"); at != std::string::npos;
         at = body.find("finish_worker()", at + 1)) {
        const auto before = body.rfind("lock.", at);
        require(before != std::string::npos, "finish_worker must follow an explicit lock transition");
        require(body.compare(before, 13, "lock.unlock()") == 0,
            "finish_worker must wait with ngxMutex released");
        ++waits;
    }
    require(waits >= 2, "both worker waits must be visible to this assertion");
}
} // namespace

int main()
{
    try {
        policy();
        scale();
        gpu_import();
        mutex_discipline();
        std::printf("upscale_shared_flow: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::printf("upscale_shared_flow FAILED: %s\n", error.what());
        return 1;
    }
}
