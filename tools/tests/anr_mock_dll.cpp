// SPDX-License-Identifier: GPL-3.0-only
#include "anr_ngx_keys.h"
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <memory>
#include <new>
struct NVSDK_NGX_Handle {};

namespace {
namespace k = nr::fg::anr::keys;
using Microsoft::WRL::ComPtr;
using Result = NVSDK_NGX_Result;
using Parameter = NVSDK_NGX_Parameter;
using List = ID3D12GraphicsCommandList;
constexpr auto success = NVSDK_NGX_Result_Success;
constexpr auto failure = NVSDK_NGX_Result_FAIL_InvalidParameter;
constexpr unsigned long long frameBase = 0x100000000ull;
// The real validator admits only DLLs at least 64 KiB.
const volatile unsigned char imageSizePad[65536]{1};

bool flag(const wchar_t* name)
{
    wchar_t value[2]{};
    return GetEnvironmentVariableW(name, value, 2) == 1 && value[0] == L'1';
}

void append(const char* text, DWORD length)
{
    wchar_t path[1024]{};
    const auto size = GetEnvironmentVariableW(L"OBS_NR_TEST_ANR_TRACE", path, ARRAYSIZE(path));
    if (!size || size >= ARRAYSIZE(path)) return;
    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written{};
    WriteFile(file, text, length, &written, nullptr);
    CloseHandle(file);
}

template<size_t N> void event(const char (&text)[N]) { append(text, static_cast<DWORD>(N - 1)); }

template<class T> bool has(const Parameter& p, const char* key, T expected)
{
    T actual{};
    return p.Get(key, &actual) == success && actual == expected;
}

struct Feature : NVSDK_NGX_Handle {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12DescriptorHeap> cpu, gpu;
    UINT step{}, cursor{};
    ID3D12Resource* disable{};

    bool initialize(List* list)
    {
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 160, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&cpu)))) return false;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&gpu)))) return false;
        step = device->GetDescriptorHandleIncrementSize(desc.Type);
        return true;
    }

    void clear(List* list, ID3D12Resource* image, ID3D12Resource* status, unsigned sub)
    {
        auto cpuSlot = cpu->GetCPUDescriptorHandleForHeapStart();
        auto gpuSlot = gpu->GetCPUDescriptorHandleForHeapStart();
        auto shaderSlot = gpu->GetGPUDescriptorHandleForHeapStart();
        cpuSlot.ptr += cursor * step; gpuSlot.ptr += cursor * step; shaderSlot.ptr += cursor * step;
        device->CreateUnorderedAccessView(image, nullptr, nullptr, cpuSlot);
        device->CopyDescriptorsSimple(1, gpuSlot, cpuSlot, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12DescriptorHeap* heaps[]{gpu.Get()};
        list->SetDescriptorHeaps(1, heaps);
        const float color[]{0.125f * static_cast<float>(sub), 0.25f, 0.75f, 1.f};
        list->ClearUnorderedAccessViewFloat(shaderSlot, cpuSlot, image, color, 0, nullptr);
        cpuSlot.ptr += step; gpuSlot.ptr += step; shaderSlot.ptr += step;
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 1;
        device->CreateUnorderedAccessView(status, nullptr, &desc, cpuSlot);
        device->CopyDescriptorsSimple(1, gpuSlot, cpuSlot, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!flag(L"OBS_NR_TEST_ANR_DISABLE") || sub == 2) {
            const UINT value[4]{};
            list->ClearUnorderedAccessViewUint(shaderSlot, cpuSlot, status, value, 0, nullptr);
        }
        cursor += 2;
    }
};
} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) event("attach\n");
    return TRUE;
}

extern "C" {
__declspec(dllexport) Result NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(unsigned long long id, const wchar_t* folder,
    ID3D12Device* device, NVSDK_NGX_Version version, const NVSDK_NGX_FeatureCommonInfo* common)
{
    event("init\n");
    return imageSizePad[0] + imageSizePad[65535] == 1 && id == 1 && folder && folder[0] && device &&
        version == NVSDK_NGX_Version_API && !common ? success : failure;
}

__declspec(dllexport) Result NVSDK_CONV NVSDK_NGX_D3D12_PopulateParameters_Impl(Parameter* p)
{
    p->Set(k::multiFrameCountMax, flag(L"OBS_NR_TEST_ANR_CAPABILITY") ? 0u : 3u);
    return success;
}

__declspec(dllexport) Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(List* list, NVSDK_NGX_Feature feature,
    Parameter* p, NVSDK_NGX_Handle** handle)
{
    if (feature != k::feature || !has(*p, k::width, 640u) || !has(*p, k::height, 360u) ||
        !has(*p, k::internalWidth, 640u) || !has(*p, k::internalHeight, 360u) ||
        !has(*p, k::backbufferFormat, static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM)) ||
        !has(*p, k::dynamicResolution, 0u) || !has(*p, k::creationNodeMask, 1u) ||
        !has(*p, k::visibilityNodeMask, 1u) || !has(*p, k::resourceAlwaysProvidedFlags, 0x07u) ||
        !has(*p, k::resourceNeverProvidedFlags, 0xB8u)) return failure;
    auto state = std::unique_ptr<Feature>(new (std::nothrow) Feature);
    if (!state || !state->initialize(list)) return failure;
    *handle = state.release();
    event("create flags=07/B8 size=640x360\n");
    return success;
}

__declspec(dllexport) Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(List* list, const NVSDK_NGX_Handle* handle,
    Parameter* p, PFN_NVSDK_NGX_ProgressCallback)
{
    ID3D12Resource *color{}, *motion{}, *depth{}, *output{}, *disable{};
    unsigned long long serial{};
    unsigned count{}, sub{}, reset{};
    if (p->Get(k::backbufferFrameId, &serial) != success || serial <= frameBase ||
        p->Get(k::multiFrameCount, &count) != success || p->Get(k::multiFrameIndex, &sub) != success ||
        p->Get(k::reset, &reset) != success || !sub || sub > count || count > 3 ||
        p->Get(k::backbuffer, &color) != success || !color ||
        p->Get(k::mvecs, &motion) != success || !motion || motion->GetDesc().Format != DXGI_FORMAT_R16G16_FLOAT ||
        p->Get(k::depth, &depth) != success || !depth || depth->GetDesc().Format != DXGI_FORMAT_R32_FLOAT ||
        p->Get(k::outputInterpolated, &output) != success || !output ||
        p->Get(k::outputDisableInterpolation, &disable) != success || !disable ||
        disable->GetDesc().Width != 4 || !has(*p, k::mvecScaleX, 1.f) || !has(*p, k::mvecScaleY, 1.f) ||
        !has(*p, k::mvecInvalidValue, FLT_MAX)) return failure;
    auto* state = const_cast<Feature*>(static_cast<const Feature*>(handle));
    if (state->cursor >= 160 || (sub > 1 && state->disable != disable)) return failure;
    state->disable = disable;
    char message[100]{};
    const int len = sprintf_s(message, "eval frame=%llu count=%u sub=%u reset=%u scale=1,1 resources=5\n",
        serial, count, sub, reset);
    if (len > 0) append(message, static_cast<DWORD>(len));
    if (!reset && flag(L"OBS_NR_TEST_ANR_FAULT")) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    if (!reset && flag(L"OBS_NR_TEST_ANR_EVAL_RETURN")) return failure;
    state->clear(list, output, disable, sub);
    return success;
}

__declspec(dllexport) Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* handle)
{
    if (flag(L"OBS_NR_TEST_ANR_RELEASE_RETURN")) {
        event("release_refused\n");
        return failure;
    }
    delete static_cast<Feature*>(handle);
    event("release\n");
    return success;
}

__declspec(dllexport) Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown1(ID3D12Device*)
{
    if (flag(L"OBS_NR_TEST_ANR_SHUTDOWN_RETURN")) {
        event("shutdown_refused\n");
        return failure;
    }
    event("shutdown\n");
    return success;
}
}
