// SPDX-License-Identifier: GPL-3.0-only
// Test-only MFGUnlocker.dll stand-in. It implements the exact version-1
// MfgXeMFGProviderApi so the production validator, runtime and backend can be
// exercised without any community or Intel binary. Every call is traceable and
// the refusal/fault/result cases are driven only by environment variables.
#include "mfg_xemfg_provider.h"
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
namespace {
using Microsoft::WRL::ComPtr;
using Result = int32_t;
constexpr Result success = NVSDK_NGX_Result_Success;
// The validator rejects images below 16 KiB; a real shim carries far more code
// than this fixture, so the pad only keeps the admitted size realistic.
const volatile unsigned char imageSizePad[65536]{1};

bool flag(const wchar_t* name)
{
    wchar_t value[2]{};
    return GetEnvironmentVariableW(name, value, 2) == 1 && value[0] == L'1';
}

std::wstring environment(const wchar_t* name)
{
    wchar_t value[512]{};
    const DWORD length = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    if (!length || length >= ARRAYSIZE(value)) return {};
    return std::wstring(value, length);
}

std::string narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    char value[1024]{};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, value, sizeof value, nullptr, nullptr);
    return bytes > 1 ? std::string(value) : std::string{};
}

unsigned number(const wchar_t* name, unsigned fallback)
{
    const auto text = environment(name);
    if (text.empty()) return fallback;
    wchar_t* end{};
    const auto value = wcstoul(text.c_str(), &end, 10);
    return *end ? fallback : static_cast<unsigned>(value);
}

void append(const char* text, DWORD length)
{
    const auto path = environment(L"OBS_NR_TEST_XEMFG_TRACE");
    if (path.empty()) return;
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written{};
    WriteFile(file, text, length, &written, nullptr);
    CloseHandle(file);
}

void event(const char* text)
{
    append(text, static_cast<DWORD>(std::strlen(text)));
}

template<class... Args>
void eventf(const char* format, Args... args)
{
    char line[256]{};
    const int length = sprintf_s(line, format, args...);
    if (length > 0) append(line, static_cast<DWORD>(length));
}

struct Feature {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12DescriptorHeap> cpu, gpu;
    UINT step{}, cursor{};
    bool initialize(ID3D12GraphicsCommandList* list)
    {
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&cpu)))) return false;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&gpu)))) return false;
        step = device->GetDescriptorHandleIncrementSize(desc.Type);
        return true;
    }
    void clear(ID3D12GraphicsCommandList* list, ID3D12Resource* output, unsigned index)
    {
        if (cursor >= 64) return;
        auto cpuSlot = cpu->GetCPUDescriptorHandleForHeapStart();
        cpuSlot.ptr += static_cast<SIZE_T>(cursor) * step;
        auto copySlot = gpu->GetCPUDescriptorHandleForHeapStart();
        copySlot.ptr += static_cast<SIZE_T>(cursor) * step;
        auto shaderSlot = gpu->GetGPUDescriptorHandleForHeapStart();
        shaderSlot.ptr += static_cast<SIZE_T>(cursor) * step;
        device->CreateUnorderedAccessView(output, nullptr, nullptr, cpuSlot);
        device->CopyDescriptorsSimple(1, copySlot, cpuSlot, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12DescriptorHeap* heaps[]{gpu.Get()};
        list->SetDescriptorHeaps(1, heaps);
        // Exactly representable UNORM8 values: a float clear goes through the
        // adapter's tie rule, and fractions of 255 land on x.5, so WARP and
        // hardware disagree by one LSB on the shared fixture colour.
        const float color[]{static_cast<float>(index) * (64.f / 255.f), 128.f / 255.f, 192.f / 255.f, 1.f};
        list->ClearUnorderedAccessViewFloat(shaderSlot, cpuSlot, output, color, 0, nullptr);
        ++cursor;
    }
};

template<class T>
bool read(const NVSDK_NGX_Parameter& parameters, const char* name, T& value)
{
    return parameters.Get(name, &value) == success;
}
} // namespace

extern "C" {
__declspec(dllexport) const MfgXeMFGProviderApi* MfgXeMFGProviderGetApi(uint32_t version);

uint32_t MaxGeneratedFrames()
{
    const auto count = number(L"OBS_NR_TEST_XEMFG_MAX_GENERATED", 3);
    eventf("max_generated=%u\n", count);
    return count;
}

uint32_t Refusal(char* reason, uint32_t capacity)
{
    event("refusal\n");
    const auto text = narrow(environment(L"OBS_NR_TEST_XEMFG_REFUSAL"));
    if (text.empty()) return 0;
    if (reason && capacity) {
        strncpy_s(reason, capacity, text.c_str(), _TRUNCATE);
        reason[capacity - 1] = '\0';
    }
    return 1;
}

int32_t D3D12Init(ID3D12Device* device)
{
    event("init\n");
    if (!device) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto forced = environment(L"OBS_NR_TEST_XEMFG_INIT_RETURN");
    return forced.empty() ? success : static_cast<int32_t>(number(L"OBS_NR_TEST_XEMFG_INIT_RETURN", 0));
}

int32_t D3D12CreateFeature(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters, void** feature)
{
    event("create\n");
    if (!list || !parameters || !feature) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *feature = nullptr;
    if (number(L"OBS_NR_TEST_XEMFG_CREATE_RETURN", 0)) return -3;
    auto state = std::unique_ptr<Feature>(new (std::nothrow) Feature);
    if (!state || !state->initialize(list)) return NVSDK_NGX_Result_Fail;
    *feature = state.release();
    return success;
}

int32_t D3D12Evaluate(ID3D12GraphicsCommandList* list, void* handle, NVSDK_NGX_Parameter* parameters)
{
    if (!list || !handle || !parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    ID3D12Resource *color{}, *motion{}, *depth{}, *output{};
    unsigned long long serial{};
    unsigned count{}, index{}, reset{};
    float scaleX{}, scaleY{}, aspect{}, nearValue{}, farValue{}, fov{};
    unsigned rectBase{}, rectWidth{}, rectHeight{};
    if (!read(*parameters, "DLSSG.Backbuffer", color) || !color ||
        !read(*parameters, "DLSSG.MVecs", motion) || !motion ||
        motion->GetDesc().Format != DXGI_FORMAT_R16G16_FLOAT ||
        !read(*parameters, "DLSSG.Depth", depth) || !depth ||
        depth->GetDesc().Format != DXGI_FORMAT_R32_FLOAT ||
        !read(*parameters, "DLSSG.OutputInterpolated", output) || !output ||
        !read(*parameters, "DLSSG.BackbufferFrameID", serial) ||
        !read(*parameters, "DLSSG.MultiFrameCount", count) || !count || count > 3 ||
        !read(*parameters, "DLSSG.MultiFrameIndex", index) || !index || index > count ||
        !read(*parameters, "DLSSG.Reset", reset) || reset > 1 ||
        !read(*parameters, "DLSSG.MvecScaleX", scaleX) || scaleX != 1.f ||
        !read(*parameters, "DLSSG.MvecScaleY", scaleY) || scaleY != 1.f ||
        !read(*parameters, "DLSSG.CameraAspectRatio", aspect) || aspect <= 0.f ||
        !read(*parameters, "DLSSG.CameraNear", nearValue) || nearValue <= 0.f ||
        !read(*parameters, "DLSSG.CameraFar", farValue) || farValue <= nearValue ||
        !read(*parameters, "DLSSG.CameraFOV", fov) || fov <= 0.f ||
        !read(*parameters, "DLSSG.DepthSubrectBaseX", rectBase) ||
        !read(*parameters, "DLSSG.DepthSubrectWidth", rectWidth) || !rectWidth ||
        !read(*parameters, "DLSSG.DepthSubrectHeight", rectHeight) || !rectHeight) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    (void)rectBase;
    const auto colorDesc = color->GetDesc();
    if (output->GetDesc().Format != colorDesc.Format || colorDesc.Width != rectWidth || colorDesc.Height != rectHeight)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    eventf("eval frame=%llu count=%u index=%u reset=%u\n", serial, count, index, reset);
    if (flag(L"OBS_NR_TEST_XEMFG_FAULT") && index >= number(L"OBS_NR_TEST_XEMFG_FAULT_INDEX", 2))
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    if (number(L"OBS_NR_TEST_XEMFG_EVAL_RETURN", 0)) return -4;
    static_cast<Feature*>(handle)->clear(list, output, index);
    return success;
}

int32_t D3D12ReleaseFeature(void* handle)
{
    if (!handle) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (number(L"OBS_NR_TEST_XEMFG_RELEASE_RETURN", 0)) {
        event("release_refused\n");
        return -5;
    }
    delete static_cast<Feature*>(handle);
    event("release\n");
    return success;
}

int32_t D3D12Shutdown()
{
    if (number(L"OBS_NR_TEST_XEMFG_SHUTDOWN_RETURN", 0)) {
        event("shutdown_refused\n");
        return -6;
    }
    event("shutdown\n");
    return success;
}

const MfgXeMFGProviderApi* MfgXeMFGProviderGetApi(uint32_t version)
{
    event("getapi\n");
    if (version != MFG_XEMFG_PROVIDER_API_VERSION) return nullptr;
    static const MfgXeMFGProviderApi api{
        sizeof(MfgXeMFGProviderApi), MFG_XEMFG_PROVIDER_API_VERSION,
        nullptr, nullptr, nullptr, nullptr, nullptr, MaxGeneratedFrames, Refusal,
        D3D12Init, D3D12CreateFeature, D3D12Evaluate, D3D12ReleaseFeature, D3D12Shutdown};
    return &api;
}
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) event("attach\n");
    else if (reason == DLL_PROCESS_DETACH) event("detach\n");
    return TRUE;
}
