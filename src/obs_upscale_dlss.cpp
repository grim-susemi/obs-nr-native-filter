// SPDX-License-Identifier: GPL-3.0-only
#include "obs_upscale_dlss.h"

#ifdef _WIN32
#include "nr_runtime.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <softpub.h>
#include <wintrust.h>
#include <wrl/client.h>
#include <wincrypt.h>
#include <nvOpticalFlowD3D11.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT64 invalidFence = std::numeric_limits<UINT64>::max();
constexpr DWORD drainMs = 1500;
constexpr UINT64 maxHistoryGapNs = 100'000'000;
constexpr char projectId[] = "f890e249-34d1-4c7f-9069-37d8c312a61e";

#ifdef OBS_NR_VFG_QA_ACTION
// Fault injection only: 110 ms after every 24th completed evaluation.
bool qa_pacing_stall() noexcept
{
    static const bool enabled = GetEnvironmentVariableW(L"OBS_NR_QA_DLSS_STALL", nullptr, 0) > 0;
    return enabled;
}

#endif

#include "obs_upscale_dlss_shader.h"

void check(HRESULT hr, const char* where)
{
    if (FAILED(hr)) {
        blog(LOG_WARNING, "obs-nr-upscale: %s failed (0x%08lx)", where, static_cast<unsigned long>(hr));
        throw std::runtime_error(where);
    }
}

void check_ngx(NVSDK_NGX_Result code, const char* where)
{
    if (NVSDK_NGX_FAILED(code)) {
        blog(LOG_INFO, "obs-nr-upscale: %s failed (0x%08x)", where, static_cast<unsigned>(code));
        throw std::runtime_error(where);
    }
}

// Guard the calls that may execute vendor code with live device/GPU state. An
// SEH fault never counts as release or successful completion.
template<typename Fn, typename... Args>
__declspec(noinline) NVSDK_NGX_Result ngx_call(Fn fn, Args... args) noexcept
{
    __try {
        return fn(args...);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }
}

struct NgxD3d11 {
    // The installed CORE exports differ from the static SDK facade: the
    // version comes before FeatureCommonInfo, and shutdown writes a device count.
    using Init = NVSDK_NGX_Result (NVSDK_CONV *)(const char*, NVSDK_NGX_EngineType, const char*,
        const wchar_t*, ID3D11Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
    using Shutdown = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D11Device*, unsigned*);

    Init init = nullptr;
    decltype(&NVSDK_NGX_D3D11_GetCapabilityParameters) capabilities = nullptr;
    decltype(&NVSDK_NGX_D3D11_AllocateParameters) allocate = nullptr;
    decltype(&NVSDK_NGX_D3D11_DestroyParameters) destroy = nullptr;
    decltype(&NVSDK_NGX_D3D11_CreateFeature) create = nullptr;
    decltype(&NVSDK_NGX_D3D11_EvaluateFeature_C) evaluate = nullptr;
    decltype(&NVSDK_NGX_D3D11_ReleaseFeature) release = nullptr;
    Shutdown shutdown = nullptr;

    template<typename Fn>
    static Fn symbol(HMODULE module, const char* name)
    {
        const auto address = GetProcAddress(module, name);
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
            info.AllocationBase != module)
            throw std::runtime_error(std::string("NVIDIA export unavailable: ") + name);
        return reinterpret_cast<Fn>(address);
    }

    void bind(HMODULE module)
    {
        init = symbol<Init>(module, "NVSDK_NGX_D3D11_Init_ProjectID");
        capabilities = symbol<decltype(capabilities)>(module, "NVSDK_NGX_D3D11_GetCapabilityParameters");
        allocate = symbol<decltype(allocate)>(module, "NVSDK_NGX_D3D11_AllocateParameters");
        destroy = symbol<decltype(destroy)>(module, "NVSDK_NGX_D3D11_DestroyParameters");
        create = symbol<decltype(create)>(module, "NVSDK_NGX_D3D11_CreateFeature");
        evaluate = symbol<decltype(evaluate)>(module, "NVSDK_NGX_D3D11_EvaluateFeature");
        release = symbol<decltype(release)>(module, "NVSDK_NGX_D3D11_ReleaseFeature");
        shutdown = symbol<Shutdown>(module, "NVSDK_NGX_D3D11_Shutdown1");
    }
};

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::filesystem::path path_from_obs(const std::string& value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

struct Config {
    uintptr_t renderer;
    LUID adapter;
    UINT width, height, outWidth, outHeight;
    DXGI_FORMAT format;
    uint32_t model = 1;
    std::string runtime;

    bool operator==(const Config& b) const noexcept
    {
        return renderer == b.renderer && adapter.LowPart == b.adapter.LowPart &&
               adapter.HighPart == b.adapter.HighPart && width == b.width && height == b.height &&
               outWidth == b.outWidth && outHeight == b.outHeight && format == b.format &&
               model == b.model && runtime == b.runtime;
    }
};

struct Capture {
    UINT64 epoch = 0, fence = 0;
    UINT64 time = 0, interval = 0;
};

struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

Image make_image(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                 bool uav, Handle* shared = nullptr)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    if (shared) {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    }
    Image image;
    check(device->CreateTexture2D(&desc, nullptr, &image.texture), "SR texture");
    check(device->CreateShaderResourceView(image.texture.Get(), nullptr, &image.srv), "SR texture view");
    if (uav)
        check(device->CreateUnorderedAccessView(image.texture.Get(), nullptr, &image.uav), "SR writable view");
    if (shared) {
        ComPtr<IDXGIResource1> resource;
        check(image.texture.As(&resource), "SR shared resource");
        check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &shared->value), "SR texture handle");
    }
    return image;
}

ComPtr<ID3DBlob> shader_code(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, error;
    const D3D_SHADER_MACRO compute[]{{"COMPUTE", "1"}, {nullptr, nullptr}};
    const auto hr = D3DCompile(shader, sizeof(shader) - 1, "obs_upscale_dlss",
                               target[0] == 'c' ? compute : nullptr, nullptr, entry,
                               target, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
                               0, &code, &error);
    if (error)
        blog(FAILED(hr) ? LOG_WARNING : LOG_INFO, "obs-nr-upscale: %s", static_cast<const char*>(error->GetBufferPointer()));
    check(hr, "SR shader compile");
    return code;
}

ComPtr<ID3D11ComputeShader> program(ID3D11Device* device, const char* entry)
{
    const auto code = shader_code(entry, "cs_5_0");
    ComPtr<ID3D11ComputeShader> result;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &result),
          "SR compute shader");
    return result;
}

// One admitted D3D11 CORE/device per process. Per-filter resources and
// histories are independent, but the private immediate context is shared.
std::mutex ngxMutex;

std::filesystem::path runtime_path(const std::string& overridePath)
{
    if (!overridePath.empty()) {
        auto path = path_from_obs(overridePath);
        if (!path.is_absolute() || _wcsicmp(path.filename().c_str(), L"nvngx_dlss.dll"))
            throw std::runtime_error("NGX SR runtime must be an absolute nvngx_dlss.dll path");
        std::error_code error;
        const auto resolved = std::filesystem::canonical(path, error);
        if (!error)
            return resolved;
        if (error != std::errc::no_such_file_or_directory)
            throw std::runtime_error("NVIDIA SR runtime is missing or inaccessible");
    }
    std::array<wchar_t, 32768> exe{};
    const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size())
        throw std::runtime_error("OBS SR executable path is unavailable");
    return std::filesystem::path(exe.data()).parent_path() / L"dlss" / L"nvngx_dlss.dll";
}

void admit_runtime(const std::filesystem::path& path, Handle& pinned)
{
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("NVIDIA library is missing or inaccessible");
    pinned.value = file; // Keep the admitted file immutable while NGX can use it.
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!GetFileInformationByHandle(file, &identity) ||
        (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("NVIDIA library is not a regular file");
    WINTRUST_FILE_INFO info{};
    info.cbStruct = sizeof(info);
    info.pcwszFilePath = path.c_str();
    info.hFile = file;
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &trust);
    const auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
    const auto* signer = provider ? WTHelperGetProvSignerFromChain(const_cast<CRYPT_PROVIDER_DATA*>(provider),
                                                                   0, FALSE, 0) : nullptr;
    std::array<wchar_t, 128> subject{};
    if (signer && signer->csCertChain && signer->pasCertChain[0].pCert)
        CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                           subject.data(), static_cast<DWORD>(subject.size()));
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust);
    if (status != ERROR_SUCCESS || std::wstring_view(subject.data()) != L"NVIDIA Corporation")
        throw std::runtime_error("NVIDIA library signature rejected");
}

bool image_matches(HMODULE module, const std::filesystem::path& path)
{
    std::array<wchar_t, 32768> image{};
    const auto length = GetModuleFileNameW(module, image.data(), static_cast<DWORD>(image.size()));
    std::error_code error;
    return length && length < image.size() && std::filesystem::equivalent(path, image.data(), error) && !error;
}

#include "obs_upscale_dlss_flow.h"

struct RenderSize {
    UINT width = 0, height = 0;
    NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_DLAA;
};

struct NgxCore {
    const LUID adapter;
    const std::filesystem::path runtime;
    const std::wstring cachePath, dllPath;
    const wchar_t* dllLocations[1]{};
    NVSDK_NGX_FeatureCommonInfo info{};
    Handle runtimeFile, driverFile;
    HMODULE driver = nullptr;
    NgxD3d11 ngx;
    ComPtr<ID3D11Device5> device;
    ComPtr<ID3D11DeviceContext4> commands;
    bool initialized = false, unsafe = false;

    NgxCore(LUID luid, const std::filesystem::path& path, std::wstring cache)
        : adapter(luid), runtime(path), cachePath(std::move(cache)), dllPath(path.parent_path().wstring()) {}

    void load_driver()
    {
        const auto expected = nr::runtime::resolve_driver();
        if (!expected.signatureVerified)
            throw std::runtime_error("NGX SR CORE signature rejected");
        const auto& path = expected.identity.canonicalPath;
        const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("NGX SR CORE could not be pinned");
        driverFile.value = file;
        const auto current = nr::runtime::resolve_driver();
        if (!current.signatureVerified || current.identity.canonicalPath != path ||
            current.identity.sha256 != expected.identity.sha256)
            throw std::runtime_error("NGX SR active driver changed during admission");
        driver = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!driver || !image_matches(driver, path))
            throw std::runtime_error("NGX SR CORE image could not be loaded from the active driver package");
        ngx.bind(driver);
    }

    void open(IDXGIAdapter1* selected)
    {
        admit_runtime(runtime, runtimeFile);
        constexpr D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D11Device> native;
        ComPtr<ID3D11DeviceContext> immediate;
        check(D3D11CreateDevice(selected, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                &level, 1, D3D11_SDK_VERSION, &native, nullptr, &immediate), "SR private D3D11 device");
        check(native.As(&device), "SR fence device");
        check(immediate.As(&commands), "SR fence context");
        std::filesystem::create_directories(cachePath);
        dllLocations[0] = dllPath.c_str();
        info.PathListInfo.Length = 1;
        info.PathListInfo.Path = dllLocations;
        load_driver();
        if (const auto image = GetModuleHandleW(L"nvngx_dlss.dll"); image && !image_matches(image, runtime))
            throw std::runtime_error("A different NGX SR runtime is already loaded");
        unsafe = true;
        const auto result = ngx_call(ngx.init, projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                     cachePath.c_str(), native.Get(), NVSDK_NGX_Version_API, &info);
        check_ngx(result, "NGX SR initialization");
        initialized = true;
        unsafe = false;
    }
};

std::weak_ptr<NgxCore> currentCore;

NVSDK_NGX_Result choose_render_size(NVSDK_NGX_Parameter* caps, const Config* c, RenderSize* chosen)
{
    int available = 0;
    if (NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    *chosen = {c->width, c->height};
    if (c->width == c->outWidth && c->height == c->outHeight)
        return NVSDK_NGX_Result_Success;

    void* entry = nullptr;
    if (NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &entry)) || !entry)
        return NVSDK_NGX_Result_FAIL_OutOfDate;
    const auto optimal = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(entry);
    constexpr std::array modes{NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
                               NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    UINT best = UINT_MAX;
    caps->Set(NVSDK_NGX_Parameter_Width, c->outWidth);
    caps->Set(NVSDK_NGX_Parameter_Height, c->outHeight);
    caps->Set(NVSDK_NGX_Parameter_RTXValue, 0);
    for (const auto mode : modes) {
        caps->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(mode));
        UINT w = 0, h = 0;
        if (NVSDK_NGX_FAILED(optimal(caps)) ||
            NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_OutWidth, &w)) ||
            NVSDK_NGX_FAILED(caps->Get(NVSDK_NGX_Parameter_OutHeight, &h)) || !w || !h)
            continue;
        const auto bound = [caps](const char* key, UINT native) {
            UINT size = 0;
            return NVSDK_NGX_SUCCEED(caps->Get(key, &size)) && size ? size : native;
        };
        if (c->width < bound(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, w) ||
            c->width > bound(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, w) ||
            c->height < bound(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, h) ||
            c->height > bound(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, h))
            continue;
        const UINT distance = (c->width > w ? c->width - w : w - c->width) +
                              (c->height > h ? c->height - h : h - c->height);
        if (distance < best) {
            *chosen = {w, h, mode};
            best = distance;
        }
    }
    return best == UINT_MAX ? NVSDK_NGX_Result_FAIL_InvalidParameter : NVSDK_NGX_Result_Success;
}

struct Session {
    const Config config;
    std::shared_ptr<NgxCore> core;
    ComPtr<ID3D11Device5> device;
    ComPtr<ID3D11DeviceContext4> commands;
    ComPtr<ID3D11Fence> transfer, worker;
    Handle inputHandle, outputHandle, transferHandle, event;
    Image input, output, evaluated, motion, depth, cutFlag;
    OpticalFlow flow;
    std::array<Image, 2> colors;
    ComPtr<ID3D11ComputeShader> prepare, densify, detectCut, compose;
    ComPtr<ID3D11Texture2D> cutReadback;
    ComPtr<ID3D11Buffer> motionUnits;
    ComPtr<ID3D11SamplerState> sampler;
    float inputPerOutputX = 1.0f, inputPerOutputY = 1.0f;
    NVSDK_NGX_Parameter* caps = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    bool unresolved = false;
    UINT64 submitted = 0;
    Capture previous{};
    size_t nextColor = 0;
    bool resetFlowHints = true;

    explicit Session(const Config& c) : config(c) {}

    bool wait(ID3D11Fence* f, UINT64 value) noexcept
    {
        const auto complete = [f, value] {
            const auto actual = f->GetCompletedValue();
            return actual != invalidFence && actual >= value;
        };
        if (complete())
            return true;
        if (f->GetCompletedValue() == invalidFence || !ResetEvent(event.value) ||
            FAILED(f->SetEventOnCompletion(value, event.value)))
            return false;
        return WaitForSingleObject(event.value, drainMs) == WAIT_OBJECT_0 && complete();
    }

    // Arm the event before Signal/Flush. GPU work issued before a failed
    // Signal, timeout or device removal is never inferred to be complete.
    bool finish_worker() noexcept
    {
        if (submitted >= invalidFence - 1 || !ResetEvent(event.value))
            return false;
        const auto value = ++submitted;
        if (FAILED(worker->SetEventOnCompletion(value, event.value)) || FAILED(commands->Signal(worker.Get(), value)))
            return false;
        commands->Flush();
        if (WaitForSingleObject(event.value, drainMs) != WAIT_OBJECT_0)
            return false;
        const auto actual = worker->GetCompletedValue();
        return actual != invalidFence && actual >= value && SUCCEEDED(device->GetDeviceRemovedReason());
    }

    void open(const std::string& cache)
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "SR DXGI factory");
        check(factory->EnumAdapterByLuid(config.adapter, IID_PPV_ARGS(&adapter)), "SR adapter lookup");
        DXGI_ADAPTER_DESC1 adapterDesc{};
        check(adapter->GetDesc1(&adapterDesc), "SR adapter description");
        if (adapterDesc.VendorId != 0x10de || (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            throw std::runtime_error("SR requires an NVIDIA hardware renderer");
        const auto runtime = runtime_path(config.runtime);
        const auto cachePath = std::filesystem::absolute(path_from_obs(cache)).wstring();
        const std::lock_guard lock(ngxMutex);
        if (auto existing = currentCore.lock()) {
            std::error_code error;
            if (existing->unsafe || existing->adapter.LowPart != config.adapter.LowPart ||
                existing->adapter.HighPart != config.adapter.HighPart || existing->cachePath != cachePath ||
                !std::filesystem::equivalent(existing->runtime, runtime, error) || error)
                throw std::runtime_error("NGX SR requires the active admitted CORE session configuration");
            core = std::move(existing);
        } else {
            core = std::make_shared<NgxCore>(config.adapter, runtime, cachePath);
            currentCore = core;
            core->open(adapter.Get());
        }
        device = core->device;
        commands = core->commands;
        event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event.value)
            throw std::runtime_error("SR completion event");
        check(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&transfer)), "SR transfer fence");
        check(transfer->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &transferHandle.value), "SR fence handle");
        check(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&worker)), "SR completion fence");

        const UINT w = config.width, h = config.height, ow = config.outWidth, oh = config.outHeight;
        constexpr auto fp = DXGI_FORMAT_R16G16B16A16_FLOAT;
        input = make_image(device.Get(), w, h, config.format, false, &inputHandle);
        output = make_image(device.Get(), ow, oh, fp, true, &outputHandle);
        evaluated = make_image(device.Get(), ow, oh, fp, true);
        for (auto& color : colors)
            color = make_image(device.Get(), w, h, fp, true);
        motion = make_image(device.Get(), w, h, DXGI_FORMAT_R16G16_FLOAT, true);
        depth = make_image(device.Get(), w, h, DXGI_FORMAT_R32_FLOAT, true);
        cutFlag = make_image(device.Get(), 1, 1, DXGI_FORMAT_R32_UINT, true);
        prepare = program(device.Get(), config.format == fp ? "prepare_linear" : "prepare_srgb");
        densify = program(device.Get(), "densify");
        detectCut = program(device.Get(), "detect_cut");
        compose = program(device.Get(), "compose");
        D3D11_TEXTURE2D_DESC cutDesc{};
        cutFlag.texture->GetDesc(&cutDesc);
        cutDesc.BindFlags = 0;
        cutDesc.Usage = D3D11_USAGE_STAGING;
        cutDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&cutDesc, nullptr, &cutReadback), "SR cut readback");
        const float scale[]{static_cast<float>(ow) / static_cast<float>(w),
                            static_cast<float>(oh) / static_cast<float>(h), 0.0f, 0.0f};
        inputPerOutputX = 1.0f / scale[0];
        inputPerOutputY = 1.0f / scale[1];
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(scale);
        constants.Usage = D3D11_USAGE_IMMUTABLE;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{scale, 0, 0};
        check(device->CreateBuffer(&constants, &data, &motionUnits), "SR motion units");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxAnisotropy = 1;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sd, &sampler), "SR sampler");

        // Admission failures select spatial for the entire configuration.
        // Do not alternate hardware and software vectors inside NGX history.
        unresolved = true;
        try {
            flow.open(device.Get(), commands.Get(), w, h);
        } catch (...) {
            unresolved = !finish_worker() || flow.uncertain;
            throw;
        }
        {
            if (!finish_worker())
                throw std::runtime_error("SR NVOF initialization did not complete");
            unresolved = false;
            const auto getCaps = ngx_call(core->ngx.capabilities, &caps);
            unresolved = getCaps == NVSDK_NGX_Result_FAIL_PlatformError;
            check_ngx(getCaps, "NGX SR capability map");
            if (!caps)
                throw std::runtime_error("NGX SR returned no capability map");
            const auto image = GetModuleHandleW(L"nvngx_dlss.dll");
            if (!image || !image_matches(image, runtime))
                throw std::runtime_error("NGX SR did not load the admitted runtime");
            RenderSize size;
            const auto choice = ngx_call(choose_render_size, caps, &config, &size);
            unresolved = choice == NVSDK_NGX_Result_FAIL_PlatformError;
            check_ngx(choice, "NGX SR capability/render-size selection");
            const auto allocation = ngx_call(core->ngx.allocate, &params);
            unresolved = allocation == NVSDK_NGX_Result_FAIL_PlatformError;
            check_ngx(allocation, "NGX SR parameter map");
            if (!params)
                throw std::runtime_error("NGX SR returned no parameter map");
            // Create may record commands even on failure. Discharge that work
            // before allowing any error path to release feature/device state.
            unresolved = true;
            const auto result = ngx_call(&Session::create_feature, this, &size);
            const bool complete = finish_worker();
            unresolved = !complete || result == NVSDK_NGX_Result_FAIL_PlatformError;
            check_ngx(result, "NGX SR create");
            if (!complete || !feature)
                throw std::runtime_error("NGX SR creation did not complete");
        }
        constexpr float one[]{1, 1, 1, 1};
        unresolved = true;
        commands->ClearUnorderedAccessViewFloat(depth.uav.Get(), one);
        if (!finish_worker())
            throw std::runtime_error("SR depth initialization did not complete");
        unresolved = false;
        blog(LOG_INFO, "obs-nr-upscale: NGX %s initialized (%ux%u -> %ux%u)",
             w == ow && h == oh ? "DLAA" : "DLSS SR", w, h, ow, oh);
    }

    static NVSDK_NGX_Result create_feature(Session* s, const RenderSize* size)
    {
        auto& p = *s->params;
        p.Set(NVSDK_NGX_Parameter_Width, size->width);
        p.Set(NVSDK_NGX_Parameter_Height, size->height);
        p.Set(NVSDK_NGX_Parameter_OutWidth, s->config.outWidth);
        p.Set(NVSDK_NGX_Parameter_OutHeight, s->config.outHeight);
        p.Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(size->quality));
        p.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
              NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
        p.Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
        constexpr std::array presets{NVSDK_NGX_DLSS_Hint_Render_Preset_K, NVSDK_NGX_DLSS_Hint_Render_Preset_M,
                                     NVSDK_NGX_DLSS_Hint_Render_Preset_L};
        for (const auto* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
            p.Set(key, static_cast<unsigned>(presets[s->config.model]));
        return s->core->ngx.create(s->commands.Get(), NVSDK_NGX_Feature_SuperSampling, s->params, &s->feature);
    }

    static NVSDK_NGX_Result evaluate_feature(Session* s, ID3D11Resource* color, bool reset)
    {
        auto& p = *s->params;
        p.Set(NVSDK_NGX_Parameter_Color, color);
        p.Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(s->evaluated.texture.Get()));
        p.Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(s->depth.texture.Get()));
        p.Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(s->motion.texture.Get()));
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, s->config.width);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, s->config.height);
        p.Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
        for (const auto* key : {NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y,
                                NVSDK_NGX_Parameter_Sharpness})
            p.Set(key, 0.0f);
        // Motion is stored in output pixels; normalize to render pixels for
        // NGX's input-sized, low-resolution motion vector contract.
        p.Set(NVSDK_NGX_Parameter_MV_Scale_X, s->inputPerOutputX);
        p.Set(NVSDK_NGX_Parameter_MV_Scale_Y, s->inputPerOutputY);
        for (const auto* key : {NVSDK_NGX_Parameter_DLSS_Pre_Exposure, NVSDK_NGX_Parameter_DLSS_Exposure_Scale})
            p.Set(key, 1.0f);
        return s->core->ngx.evaluate(s->commands.Get(), s->feature, s->params, nullptr);
    }

    void dispatch(ID3D11ComputeShader* shaderObject, const Image& a, const Image* b, const Image& out,
                  UINT w, UINT h, bool flowInputs = false, bool cutOutput = false)
    {
        ID3D11ShaderResourceView* srvs[]{a.srv.Get(), b ? b->srv.Get() : nullptr,
                                       flowInputs ? flow.vectors.srv.Get() : nullptr,
                                       flowInputs ? flow.cost.srv.Get() : nullptr};
        ID3D11UnorderedAccessView* uavs[]{cutOutput ? nullptr : out.uav.Get(), cutOutput ? out.uav.Get() : nullptr};
        ID3D11SamplerState* samplers[]{sampler.Get()};
        ID3D11Buffer* cb = flowInputs ? motionUnits.Get() : nullptr;
        commands->CSSetShader(shaderObject, nullptr, 0);
        commands->CSSetShaderResources(0, 4, srvs);
        commands->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        commands->CSSetSamplers(0, 1, samplers);
        commands->CSSetConstantBuffers(0, 1, &cb);
        commands->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* noSRVs[4]{};
        ID3D11UnorderedAccessView* noUAVs[2]{};
        commands->CSSetShaderResources(0, 4, noSRVs);
        commands->CSSetUnorderedAccessViews(0, 2, noUAVs, nullptr);
    }

    bool read_cut(const Image& color, const Image& prior, bool& reset)
    {
        dispatch(detectCut.Get(), color, &prior, cutFlag, 1, 1, false, true);
        commands->CopyResource(cutReadback.Get(), cutFlag.texture.Get());
        // The NGX reset flag lives on the CPU. Only the worker waits for
        // these four bytes, using the same bounded completion fence.
        if (!finish_worker())
            return false;
        D3D11_MAPPED_SUBRESOURCE map{};
        const auto hr = commands->Map(cutReadback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            // The driver may still be preparing CPU access to the staging
            // resource. Do not trust history for this frame, but keep the
            // feature alive. The next job is ordered after this job's fence.
            reset = true;
            return true;
        }
        if (FAILED(hr)) {
            blog(LOG_WARNING, "obs-nr-upscale: SR cut readback failed (0x%08lx)", static_cast<unsigned long>(hr));
            unresolved = FAILED(device->GetDeviceRemovedReason());
            return false;
        }
        reset = *static_cast<const UINT*>(map.pData) != 0;
        commands->Unmap(cutReadback.Get(), 0);
        return true;
    }

    bool run(const Capture& frame)
    {
        if (!wait(transfer.Get(), frame.fence)) {
            unresolved = true;
            return false;
        }
        const std::lock_guard lock(ngxMutex);
        if (core->unsafe) {
            unresolved = true;
            return false;
        }
        const auto w = config.width, h = config.height;
        const UINT64 cadenceLimit = std::min(maxHistoryGapNs, frame.interval + frame.interval / 2);
        bool reset = frame.epoch != previous.epoch || frame.time <= previous.time ||
                     frame.interval != previous.interval || frame.time - previous.time > cadenceLimit;
        auto& color = colors[nextColor];
        auto& prior = colors[nextColor ^ 1u];
        unresolved = true;
        dispatch(prepare.Get(), input, nullptr, color, w, h);
        flow.prepare(commands.Get(), nextColor, color, w, h);
        if (!reset && !read_cut(color, prior, reset))
            return false;
        if (reset) {
            constexpr float zero[]{0, 0, 0, 0};
            commands->ClearUnorderedAccessViewFloat(motion.uav.Get(), zero);
            resetFlowHints = true;
        } else {
            try {
                flow.execute(nextColor, resetFlowHints);
            } catch (...) {
                unresolved = !finish_worker() || flow.uncertain;
                throw;
            }
            dispatch(densify.Get(), color, &prior, motion, w, h, true);
            resetFlowHints = false;
        }
        const auto result = ngx_call(&Session::evaluate_feature, this, static_cast<ID3D11Resource*>(color.texture.Get()),
                                     reset);
        if (NVSDK_NGX_SUCCEED(result))
            dispatch(compose.Get(), evaluated, &color, output, config.outWidth, config.outHeight);
        const bool complete = finish_worker();
        unresolved = !complete || result == NVSDK_NGX_Result_FAIL_PlatformError;
        check_ngx(result, "NGX SR evaluation");
        if (!complete)
            return false;
        if (!previous.epoch)
            blog(LOG_INFO, "obs-nr-upscale: first NGX SR evaluation completed on GPU");
        previous = frame;
        nextColor ^= 1u;
        return true;
    }

    static bool retire(std::unique_ptr<Session> session) noexcept
    {
        const std::lock_guard lock(ngxMutex);
        const auto retired = [](NVSDK_NGX_Result result, const char* stage) {
            if (result == NVSDK_NGX_Result_Success)
                return true;
            blog(LOG_WARNING, "obs-nr-upscale: NGX SR %s failed (0x%08x)", stage, static_cast<unsigned>(result));
            return false;
        };
        auto& s = *session;
        if (!s.core)
            return true;
        auto& core = *s.core;
        if (s.unresolved || core.unsafe || !s.flow.close() ||
            (s.feature && !retired(ngx_call(core.ngx.release, s.feature), "feature release")) ||
            (s.params && !retired(ngx_call(core.ngx.destroy, s.params), "parameter destruction")) ||
            (s.caps && !retired(ngx_call(core.ngx.destroy, s.caps), "capability destruction"))) {
            core.unsafe = true;
            (void)session.release();
            return false;
        }
        if (s.core.use_count() > 1) {
            session.reset(); // Detach the feature owner under ngxMutex.
            return true;
        }
        if (core.initialized) {
            // A skipped or interposed shutdown cannot prove retirement.
            const auto* entry = reinterpret_cast<const unsigned char*>(core.ngx.shutdown);
            unsigned remaining = UINT_MAX;
            if (entry[0] == 0xe9 || (entry[0] == 0xff && entry[1] == 0x25) ||
                !retired(ngx_call(core.ngx.shutdown, static_cast<ID3D11Device*>(core.device.Get()), &remaining),
                         "shutdown") || remaining != 0) {
                blog(LOG_WARNING, "obs-nr-upscale: NGX SR CORE retirement unproven (registrations=%u)", remaining);
                core.unsafe = true;
                (void)session.release();
                return false;
            }
        }
        if (core.driver && !FreeLibrary(core.driver)) {
            blog(LOG_WARNING, "obs-nr-upscale: NGX SR CORE library release failed (%lu)", GetLastError());
            core.unsafe = true;
            (void)session.release();
            return false;
        }
        session.reset();
        return true;
    }
};

enum class Phase { Empty, Ready, Captured, Running, Complete, Poisoned };

DXGI_FORMAT typed_sdr(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

std::string obs_path(char* path)
{
    const std::unique_ptr<char, decltype(&bfree)> owned(path, &bfree);
    return owned ? std::string(owned.get()) : std::string{};
}
}

#ifdef OBS_NR_VFG_QA_ACTION
bool obs_upscale_dlss_qa_legacy_pacing() noexcept
{
    static const bool enabled = GetEnvironmentVariableW(L"OBS_NR_QA_DLSS_LEGACY_PACING", nullptr, 0) > 0;
    return enabled;
}
#endif

struct ObsUpscaleDlss {
    // Graphics-owned. The worker never reads these or calls OBS graphics APIs.
    gs_texture_t* presentation = nullptr;
    ComPtr<ID3D11Device> renderer;
    ComPtr<ID3D11DeviceContext4> graphics;
    ComPtr<ID3D11Texture2D> input11, output11;
    ComPtr<ID3D11Fence> transfer11;
    UINT64 boundSession = 0;
    std::atomic<UINT64> epoch{1};
    Capture displayed{};
    std::optional<Config> graphicsConfig;
    UINT64 renderTick = 0, renderInterval = 0, acceptedTick = 0;
    bool renderTemporal = false;

    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::string cache;
    bool stop = false;
    std::optional<Config> wanted;
    std::optional<Config> failed;
    Session* active = nullptr; // Borrowed under mutex; withdrawn before worker retirement.
    UINT64 sessionId = 0, access = 0;
    Phase phase = Phase::Empty;
    Capture job{};

#ifdef OBS_NR_VFG_QA_ACTION
    bool qa_fresh(const Capture& frame, UINT64 now) const noexcept
    {
        return frame.epoch == epoch.load(std::memory_order_relaxed) &&
               now >= frame.time && now - frame.time <= 80'000'000;
    }
    void qa_expire(UINT64 now) noexcept
    {
        if (displayed.epoch && !qa_fresh(displayed, now)) {
            epoch.fetch_add(1, std::memory_order_relaxed);
            displayed = {};
        }
    }
#endif

    void invalidate() noexcept
    {
        epoch.fetch_add(1, std::memory_order_relaxed);
        displayed = {};
        renderTick = renderInterval = 0;
        renderTemporal = false;
    }

    bool matches(const Capture& frame, UINT64 requested) const noexcept
    {
        return requested && frame.time == requested &&
               frame.epoch == epoch.load(std::memory_order_relaxed);
    }

    void worker_main()
    {
        std::unique_ptr<Session> session;
#ifdef OBS_NR_VFG_QA_ACTION
        unsigned qaEvaluations = 0;
#endif
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [&] {
                return stop || (session && (!wanted || *wanted != session->config ||
                                            phase == Phase::Poisoned || phase == Phase::Captured)) ||
                       (!session && wanted && failed != wanted);
            });
            if (session && (stop || !wanted || *wanted != session->config || phase == Phase::Poisoned)) {
                const bool poison = phase == Phase::Poisoned;
                const auto lastAccess = access;
                const auto config = session->config;
                active = nullptr;
                phase = Phase::Empty;
                access = 0;
                lock.unlock();
                // Retain every resource and the vendor state when ownership
                // cannot be proven retired. No detached thread survives destroy.
                if (poison || (lastAccess && !session->wait(session->transfer.Get(), lastAccess)))
                    session->unresolved = true;
                if (!Session::retire(std::move(session)))
                    blog(LOG_WARNING, "obs-nr-upscale: unresolved SR session retained for process lifetime");
                lock.lock();
                if (poison)
                    failed = config;
            }
            if (stop)
                break;
            if (!session && wanted && failed != wanted) {
                const Config request = *wanted;
                lock.unlock();
                bool opened = false;
                try {
                    session = std::make_unique<Session>(request);
                    session->open(cache);
                    opened = true;
                } catch (const std::exception& error) {
                    blog(LOG_INFO, "obs-nr-upscale: using spatial upscale: %s", error.what());
                } catch (...) {
                    blog(LOG_WARNING, "obs-nr-upscale: unexpected NGX SR initialization failure");
                }
                if (!opened && session) {
                    if (!Session::retire(std::move(session)))
                        blog(LOG_WARNING, "obs-nr-upscale: unresolved SR initialization retained");
                }
                lock.lock();
                if (opened) {
                    ++sessionId;
                    active = session.get();
                    phase = Phase::Ready;
                } else {
                    failed = request;
                }
                continue;
            }
            if (session && phase == Phase::Captured) {
                const auto frame = job;
                phase = Phase::Running;
                lock.unlock();
                bool done = false;
                try {
                    done = session->run(frame);
                } catch (const std::exception& error) {
                    blog(LOG_INFO, "obs-nr-upscale: using spatial upscale: %s", error.what());
                } catch (...) {
                    blog(LOG_WARNING, "obs-nr-upscale: unexpected NGX SR evaluation failure");
                }
#ifdef OBS_NR_VFG_QA_ACTION
                if (done && qa_pacing_stall() && ++qaEvaluations % 24u == 0) {
                    blog(LOG_INFO, "obs-nr-upscale [qa]: delaying SR completion for 110 ms");
                    std::this_thread::sleep_for(std::chrono::milliseconds(110));
                }
#endif
                lock.lock();
                if (done) {
                    phase = Phase::Complete;
                } else {
                    // A failed evaluation with a known fence can be retired
                    // normally. A rejected graphics Signal uses Poisoned.
                    failed = session->config;
                    auto retiring = std::move(session);
                    active = nullptr;
                    phase = Phase::Empty;
                    access = 0;
                    lock.unlock();
                    if (!Session::retire(std::move(retiring)))
                        blog(LOG_WARNING, "obs-nr-upscale: unresolved SR evaluation retained");
                    lock.lock();
                }
            }
        }
    }

    void bind(const Session& session)
    {
        ComPtr<ID3D11Device5> device;
        check(renderer.As(&device), "OBS SR transfer device");
        input11.Reset();
        output11.Reset();
        transfer11.Reset();
        check(device->OpenSharedResource1(session.inputHandle.value, IID_PPV_ARGS(&input11)), "OBS SR input open");
        check(device->OpenSharedResource1(session.outputHandle.value, IID_PPV_ARGS(&output11)), "OBS SR output open");
        check(device->OpenSharedFence(session.transferHandle.value, IID_PPV_ARGS(&transfer11)), "OBS SR fence open");
        gs_texture_destroy(presentation);
        presentation = gs_texture_create(session.config.outWidth, session.config.outHeight, GS_RGBA16F, 1, nullptr, 0);
        if (!presentation)
            throw std::runtime_error("OBS SR presentation texture");
        displayed = {};
        boundSession = sessionId;
    }
};

ObsUpscaleDlss* obs_upscale_dlss_create() noexcept
{
    try {
        auto backend = std::make_unique<ObsUpscaleDlss>();
        backend->cache = obs_path(obs_module_config_path("upscale-ngx"));
        if (backend->cache.empty())
            return nullptr;
        backend->thread = std::thread([owner = backend.get()] { owner->worker_main(); });
        return backend.release();
    } catch (...) {
        return nullptr;
    }
}

void obs_upscale_dlss_destroy(ObsUpscaleDlss* backend) noexcept
{
    if (!backend)
        return;
    {
        const std::lock_guard lock(backend->mutex);
        backend->stop = true;
    }
    backend->wake.notify_one();
    backend->thread.join();
    if (backend->presentation) {
        obs_enter_graphics();
        gs_texture_destroy(backend->presentation);
        obs_leave_graphics();
    }
    delete backend;
}

void obs_upscale_dlss_reset(ObsUpscaleDlss* backend) noexcept
{
    if (backend)
        backend->invalidate();
}

gs_texture_t* obs_upscale_dlss_render(ObsUpscaleDlss* backend, gs_texture_t* sdr_source,
                                    uint64_t present_tick,
                                    uint32_t output_width, uint32_t output_height,
                                    uint32_t model, const char* runtime_path) noexcept
{
    if (!backend)
        return nullptr;
    auto& b = *backend;
    try {
        if (!sdr_source || gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
            b.invalidate();
            return nullptr;
        }
        auto* source = static_cast<ID3D11Texture2D*>(gs_texture_get_obj(sdr_source));
        if (!source) {
            b.invalidate();
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        const auto format = typed_sdr(desc.Format);
        if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
            format == DXGI_FORMAT_UNKNOWN || output_width < desc.Width || output_height < desc.Height ||
            output_width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || output_height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            b.invalidate();
            return nullptr;
        }
        ID3D11Device* renderer = static_cast<ID3D11Device*>(gs_get_device_obj());
        if (!renderer)
            return nullptr;
        if (renderer != b.renderer.Get()) {
            b.invalidate();
            b.renderer = renderer;
            b.boundSession = 0;
            b.graphics.Reset();
            ComPtr<ID3D11DeviceContext> context;
            renderer->GetImmediateContext(&context);
            check(context.As(&b.graphics), "OBS SR graphics context");
        }
        if (!b.graphics)
            return nullptr;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDesc{};
        check(b.renderer.As(&dxgi), "OBS SR DXGI device");
        check(dxgi->GetAdapter(&adapter), "OBS SR renderer adapter");
        check(adapter->GetDesc(&adapterDesc), "OBS SR renderer description");
        const Config config{reinterpret_cast<uintptr_t>(renderer), adapterDesc.AdapterLuid,
                            desc.Width, desc.Height, output_width, output_height, format,
                            model <= 2 ? model : 1, runtime_path ? runtime_path : ""};
        const auto now = obs_get_video_frame_time();
        const auto interval = obs_get_frame_interval_ns();
        if (!now || !interval) {
            b.invalidate();
            return nullptr;
        }
        if (now < b.acceptedTick)
            b.acceptedTick = 0;
        if (b.graphicsConfig != config || now < b.renderTick ||
            (b.renderInterval && b.renderInterval != interval)) {
            b.invalidate();
            b.graphicsConfig = config;
        }
        // Scene, preview, and VFG draws can render the source more than
        // once per video tick. They must see the same backend and output.
        if (now == b.renderTick)
            return b.renderTemporal ? b.presentation : nullptr;
        b.renderTick = now;
        b.renderInterval = interval;
        b.renderTemporal = false;

        // Never block the graphics thread behind the worker (including its
        // resource creation, driver calls, GPU waits and retirement).
        std::unique_lock lock(b.mutex, std::try_to_lock);
        if (!lock) {
#ifdef OBS_NR_VFG_QA_ACTION
            if (obs_upscale_dlss_qa_legacy_pacing()) {
                b.qa_expire(now);
                b.renderTemporal = b.qa_fresh(b.displayed, now);
                return b.renderTemporal ? b.presentation : nullptr;
            }
#endif
            return nullptr;
        }
        if (b.wanted != config) {
            b.wanted = config;
            b.failed.reset();
            b.wake.notify_one();
        }
        if (!b.active || b.active->config != config || b.stop || b.phase == Phase::Poisoned) {
            b.displayed = {};
            return nullptr;
        }
        if (b.boundSession != b.sessionId) {
            try {
                b.bind(*b.active);
            } catch (...) {
                b.phase = Phase::Poisoned;
                b.wake.notify_one();
                throw;
            }
        }
        const bool ready = b.phase == Phase::Complete || b.phase == Phase::Ready;
        bool use = b.phase == Phase::Complete && b.matches(b.job, present_tick);
#ifdef OBS_NR_VFG_QA_ACTION
        if (obs_upscale_dlss_qa_legacy_pacing()) {
            use = b.phase == Phase::Complete && b.qa_fresh(b.job, now);
            if (!use)
                b.qa_expire(now);
        }
#endif
        if (ready && b.acceptedTick != now) {
            if (b.access >= invalidFence - 1 || b.transfer11->GetCompletedValue() == invalidFence) {
                b.phase = Phase::Poisoned;
                b.wake.notify_one();
                b.displayed = {};
                return nullptr;
            }
            if (use) {
                auto* target = static_cast<ID3D11Texture2D*>(gs_texture_get_obj(b.presentation));
                if (!target)
                    return nullptr;
                b.graphics->CopyResource(target, b.output11.Get());
                b.displayed = b.job;
            }
            b.graphics->CopyResource(b.input11.Get(), source);
            // One fence after both the last possible SR output read and the
            // next input write. The worker may touch either resource only
            // after this value completes, including during retirement.
            if (FAILED(b.graphics->Signal(b.transfer11.Get(), ++b.access))) {
                b.phase = Phase::Poisoned;
                b.wake.notify_one();
                b.displayed = {};
                return nullptr;
            }
            b.graphics->Flush();
            b.acceptedTick = now;
            b.job = {b.epoch.load(std::memory_order_relaxed), b.access, now, interval};
            b.phase = Phase::Captured;
            b.wake.notify_one();
        }
        b.renderTemporal = b.matches(b.displayed, present_tick);
#ifdef OBS_NR_VFG_QA_ACTION
        if (obs_upscale_dlss_qa_legacy_pacing())
            b.renderTemporal = b.qa_fresh(b.displayed, now);
#endif
        return b.renderTemporal ? b.presentation : nullptr;
    } catch (...) {
        b.invalidate();
        return nullptr;
    }
}

#else
ObsUpscaleDlss* obs_upscale_dlss_create() noexcept { return nullptr; }
void obs_upscale_dlss_destroy(ObsUpscaleDlss*) noexcept {}
void obs_upscale_dlss_reset(ObsUpscaleDlss*) noexcept {}
gs_texture_t* obs_upscale_dlss_render(ObsUpscaleDlss*, gs_texture_t*, uint64_t, uint32_t, uint32_t,
                                    uint32_t, const char*) noexcept { return nullptr; }
#endif
