// SPDX-License-Identifier: GPL-3.0-only
#include "native_vfg.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace nr::fg::vfg {
namespace {
namespace fs = std::filesystem;

using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUstream = void*;
using CUevent = void*;
using CUexternalMemory = void*;
using CUdeviceptr = unsigned long long;

constexpr CUresult CUDA_SUCCESS = 0;
constexpr int CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75;
constexpr int CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 76;
constexpr unsigned int CU_STREAM_NON_BLOCKING = 1;
constexpr int CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE = 5;
constexpr unsigned int CU_EXTERNAL_MEMORY_DEDICATED = 1;

struct CUDA_EXTERNAL_MEMORY_HANDLE_DESC {
    int type;
    union {
        int fd;
        struct {
            void* handle;
            const void* name;
        } win32;
        const void* nvSciBufObject;
    } handle;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};
struct CUDA_EXTERNAL_MEMORY_BUFFER_DESC {
    unsigned long long offset;
    unsigned long long size;
    unsigned int flags;
    unsigned int reserved[16];
};

static_assert(sizeof(CUDA_EXTERNAL_MEMORY_HANDLE_DESC) == 104);
static_assert(offsetof(CUDA_EXTERNAL_MEMORY_HANDLE_DESC, handle) == 8);
static_assert(offsetof(CUDA_EXTERNAL_MEMORY_HANDLE_DESC, size) == 24);
static_assert(offsetof(CUDA_EXTERNAL_MEMORY_HANDLE_DESC, flags) == 32);

template<class T> T bind_sym(HMODULE dll, const char* name)
{
    auto p = GetProcAddress(dll, name);
    if (!p) check(E_FAIL, name);
    T out{};
    static_assert(sizeof(out) == sizeof(p));
    std::memcpy(&out, &p, sizeof(p));
    return out;
}

HMODULE load_dll(const fs::path& path, const char* op)
{
    constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS;
    HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, flags);
    if (!dll) check(HRESULT_FROM_WIN32(GetLastError()), op);
    return dll;
}
} // namespace

struct NativeVfg::Impl {
    std::vector<HMODULE> libraries;

    // CUDA driver symbols
    CUresult (__stdcall* cuInit)(unsigned int){};
    CUresult (__stdcall* cuDeviceGetCount)(int*){};
    CUresult (__stdcall* cuDeviceGet)(CUdevice*, int){};
    CUresult (__stdcall* cuDeviceGetLuid)(char*, unsigned int*, CUdevice){};
    CUresult (__stdcall* cuDeviceGetAttribute)(int*, int, CUdevice){};
    CUresult (__stdcall* cuDevicePrimaryCtxRetain)(CUcontext*, CUdevice){};
    CUresult (__stdcall* cuDevicePrimaryCtxRelease)(CUdevice){};
    CUresult (__stdcall* cuCtxPushCurrent)(CUcontext){};
    CUresult (__stdcall* cuCtxPopCurrent)(CUcontext*){};
    CUresult (__stdcall* cuStreamCreate)(CUstream*, unsigned int){};
    CUresult (__stdcall* cuStreamDestroy)(CUstream){};
    CUresult (__stdcall* cuStreamSynchronize)(CUstream){};
    CUresult (__stdcall* cuEventCreate)(CUevent*, unsigned int){};
    CUresult (__stdcall* cuEventDestroy)(CUevent){};
    CUresult (__stdcall* cuEventRecord)(CUevent, CUstream){};
    CUresult (__stdcall* cuEventSynchronize)(CUevent){};
    CUresult (__stdcall* cuImportExternalMemory)(CUexternalMemory*, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC*){};
    CUresult (__stdcall* cuExternalMemoryGetMappedBuffer)(CUdeviceptr*, CUexternalMemory, const CUDA_EXTERNAL_MEMORY_BUFFER_DESC*){};
    CUresult (__stdcall* cuDestroyExternalMemory)(CUexternalMemory){};
    CUresult (__stdcall* cuMemFree)(CUdeviceptr){};

    // NvVFX & NvCVImage symbols (public C API signatures)
    int (__cdecl* createEffect)(const char*, void**){};
    void (__cdecl* destroyEffect)(void*){};
    int (__cdecl* setU32)(void*, const char*, unsigned){};
    int (__cdecl* setImage)(void*, const char*, void*){};
    int (__cdecl* setStream)(void*, const char*, CUstream){};
    int (__cdecl* loadEffect)(void*){};
    int (__cdecl* runEffect)(void*, int){};
    int (__cdecl* getVersion)(unsigned*){};

    int (__cdecl* imageCreate)(unsigned, unsigned, int, int, unsigned, unsigned, unsigned, void**){};
    void (__cdecl* imageDealloc)(void*){};
    void (__cdecl* imageDestroy)(void*){};
    int (__cdecl* imageInit)(void*, unsigned, unsigned, int, void*, int, int, unsigned, unsigned){};
    int (__cdecl* fromD3D)(DXGI_FORMAT, int*, int*, unsigned char*){};

    struct MappedBuffer {
        CUexternalMemory memory{};
        CUdeviceptr pointer{};
        void* image{};
    };
    std::array<MappedBuffer, Gpu::max_exchange> mapped{};
    size_t mappedCount{};

    CUdevice device{};
    CUcontext cudaContext{};
    CUstream stream{};
    std::array<CUevent, max_multiplier> subEvents{};
    bool retained{};
    void* effect{};
    uint32_t multiplier{2};
    unsigned version{};

    void cu_check(CUresult r, const char* op) const
    {
        if (r != CUDA_SUCCESS) check(E_FAIL, op);
    }
    void cv_check(int r, const char* op) const
    {
        if (r != 0) check(E_FAIL, op);
    }

    struct CtxGuard {
        const Impl& owner;
        bool active{};
        explicit CtxGuard(const Impl& p) : owner(p)
        {
            owner.cu_check(owner.cuCtxPushCurrent(owner.cudaContext), "cuCtxPushCurrent");
            active = true;
        }
        ~CtxGuard()
        {
            if (active) {
                CUcontext prev{};
                (void)owner.cuCtxPopCurrent(&prev);
            }
        }
    };

    ~Impl()
    {
        if (retained && cuCtxPushCurrent && cuCtxPopCurrent) {
            CUcontext prev{};
            if (cuCtxPushCurrent(cudaContext) == CUDA_SUCCESS) {
                if (stream && cuStreamSynchronize) (void)cuStreamSynchronize(stream);
                if (effect && destroyEffect) destroyEffect(effect);
                for (size_t i = 0; i < mappedCount; ++i) {
                    if (mapped[i].image && imageDestroy) imageDestroy(mapped[i].image);
                    if (mapped[i].pointer && cuMemFree) (void)cuMemFree(mapped[i].pointer);
                    if (mapped[i].memory && cuDestroyExternalMemory) (void)cuDestroyExternalMemory(mapped[i].memory);
                }
                for (auto& ev : subEvents) {
                    if (ev && cuEventDestroy) (void)cuEventDestroy(ev);
                }
                if (stream && cuStreamDestroy) (void)cuStreamDestroy(stream);
                (void)cuCtxPopCurrent(&prev);
            }
            if (cuDevicePrimaryCtxRelease) (void)cuDevicePrimaryCtxRelease(device);
        }
        for (auto it = libraries.rbegin(); it != libraries.rend(); ++it) {
            if (*it) FreeLibrary(*it);
        }
    }
};

NativeVfg::NativeVfg(const fs::path& runtimeDir, const Gpu& gpu, const Session& session)
    : impl_(std::make_unique<Impl>())
{
    auto& p = *impl_;
    p.multiplier = std::clamp<uint32_t>(session.multiplier, 2, max_multiplier);

    wchar_t sysDir[MAX_PATH]{};
    const auto n = GetSystemDirectoryW(sysDir, ARRAYSIZE(sysDir));
    if (!n || n >= ARRAYSIZE(sysDir)) check(E_FAIL, "GetSystemDirectoryW");

    HMODULE nvcuda = load_dll(fs::path(sysDir) / L"nvcuda.dll", "load nvcuda.dll");
    p.libraries.push_back(nvcuda);

    p.cuInit = bind_sym<decltype(p.cuInit)>(nvcuda, "cuInit");
    p.cuDeviceGetCount = bind_sym<decltype(p.cuDeviceGetCount)>(nvcuda, "cuDeviceGetCount");
    p.cuDeviceGet = bind_sym<decltype(p.cuDeviceGet)>(nvcuda, "cuDeviceGet");
    p.cuDeviceGetLuid = bind_sym<decltype(p.cuDeviceGetLuid)>(nvcuda, "cuDeviceGetLuid");
    p.cuDeviceGetAttribute = bind_sym<decltype(p.cuDeviceGetAttribute)>(nvcuda, "cuDeviceGetAttribute");
    p.cuDevicePrimaryCtxRetain = bind_sym<decltype(p.cuDevicePrimaryCtxRetain)>(nvcuda, "cuDevicePrimaryCtxRetain");
    p.cuDevicePrimaryCtxRelease = bind_sym<decltype(p.cuDevicePrimaryCtxRelease)>(nvcuda, "cuDevicePrimaryCtxRelease_v2");
    p.cuCtxPushCurrent = bind_sym<decltype(p.cuCtxPushCurrent)>(nvcuda, "cuCtxPushCurrent_v2");
    p.cuCtxPopCurrent = bind_sym<decltype(p.cuCtxPopCurrent)>(nvcuda, "cuCtxPopCurrent_v2");
    p.cuStreamCreate = bind_sym<decltype(p.cuStreamCreate)>(nvcuda, "cuStreamCreate");
    p.cuStreamDestroy = bind_sym<decltype(p.cuStreamDestroy)>(nvcuda, "cuStreamDestroy_v2");
    p.cuStreamSynchronize = bind_sym<decltype(p.cuStreamSynchronize)>(nvcuda, "cuStreamSynchronize");
    p.cuEventCreate = bind_sym<decltype(p.cuEventCreate)>(nvcuda, "cuEventCreate");
    p.cuEventDestroy = bind_sym<decltype(p.cuEventDestroy)>(nvcuda, "cuEventDestroy_v2");
    p.cuEventRecord = bind_sym<decltype(p.cuEventRecord)>(nvcuda, "cuEventRecord");
    p.cuEventSynchronize = bind_sym<decltype(p.cuEventSynchronize)>(nvcuda, "cuEventSynchronize");
    p.cuImportExternalMemory = bind_sym<decltype(p.cuImportExternalMemory)>(nvcuda, "cuImportExternalMemory");
    p.cuExternalMemoryGetMappedBuffer = bind_sym<decltype(p.cuExternalMemoryGetMappedBuffer)>(nvcuda, "cuExternalMemoryGetMappedBuffer");
    p.cuDestroyExternalMemory = bind_sym<decltype(p.cuDestroyExternalMemory)>(nvcuda, "cuDestroyExternalMemory");
    p.cuMemFree = bind_sym<decltype(p.cuMemFree)>(nvcuda, "cuMemFree_v2");

    p.cu_check(p.cuInit(0), "cuInit");
    int count = 0;
    p.cu_check(p.cuDeviceGetCount(&count), "cuDeviceGetCount");
    bool matched = false;
    for (int i = 0; i < count; ++i) {
        CUdevice dev = 0;
        LUID luid{};
        unsigned int mask = 0;
        p.cu_check(p.cuDeviceGet(&dev, i), "cuDeviceGet");
        if (p.cuDeviceGetLuid(reinterpret_cast<char*>(&luid), &mask, dev) == CUDA_SUCCESS &&
            luid.LowPart == session.adapter.LowPart && luid.HighPart == session.adapter.HighPart && mask != 0) {
            p.device = dev;
            matched = true;
            break;
        }
    }
    if (!matched) check(E_FAIL, "CUDA/D3D12 LUID mismatch");

    int major = 0, minor = 0;
    p.cu_check(p.cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, p.device), "compute major");
    p.cu_check(p.cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, p.device), "compute minor");
    if (major < 8 || (major == 8 && minor < 9)) {
        // Ada (8.9) and Blackwell (10+) supported natively without binary patches.
        check(E_NOTIMPL, "NVIDIA VFG native path requires Ada (RTX 40) or newer");
    }

    p.cu_check(p.cuDevicePrimaryCtxRetain(&p.cudaContext, p.device), "cuDevicePrimaryCtxRetain");
    p.retained = true;
    Impl::CtxGuard ctx(p);
    p.cu_check(p.cuStreamCreate(&p.stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
    for (uint32_t s = 1; s < p.multiplier; ++s) {
        p.cu_check(p.cuEventCreate(&p.subEvents[s], 2u /* CU_EVENT_DISABLE_TIMING */), "cuEventCreate");
    }

    const std::array<const wchar_t*, 5> deps{
        L"cudart64_12.dll", L"NVCVImage.dll", L"nvngxruntime.dll", L"NVVideoEffects.dll", L"nvVFXVideoFrameGeneration.dll"
    };
    HMODULE core{}, image{};
    for (const auto* name : deps) {
        HMODULE dll = load_dll(runtimeDir / name, "load NVIDIA VFG runtime DLL");
        p.libraries.push_back(dll);
        if (std::wcscmp(name, L"NVVideoEffects.dll") == 0) core = dll;
        if (std::wcscmp(name, L"NVCVImage.dll") == 0) image = dll;
    }

    p.createEffect = bind_sym<decltype(p.createEffect)>(core, "NvVFX_CreateEffect");
    p.destroyEffect = bind_sym<decltype(p.destroyEffect)>(core, "NvVFX_DestroyEffect");
    p.setU32 = bind_sym<decltype(p.setU32)>(core, "NvVFX_SetU32");
    p.setImage = bind_sym<decltype(p.setImage)>(core, "NvVFX_SetImage");
    p.setStream = bind_sym<decltype(p.setStream)>(core, "NvVFX_SetCudaStream");
    p.loadEffect = bind_sym<decltype(p.loadEffect)>(core, "NvVFX_Load");
    p.runEffect = bind_sym<decltype(p.runEffect)>(core, "NvVFX_Run");
    p.getVersion = bind_sym<decltype(p.getVersion)>(core, "NvVFX_GetVersion");

    p.imageCreate = bind_sym<decltype(p.imageCreate)>(image, "NvCVImage_Create");
    p.imageDealloc = bind_sym<decltype(p.imageDealloc)>(image, "NvCVImage_Dealloc");
    p.imageDestroy = bind_sym<decltype(p.imageDestroy)>(image, "NvCVImage_Destroy");
    p.imageInit = bind_sym<decltype(p.imageInit)>(image, "NvCVImage_Init");
    p.fromD3D = bind_sym<decltype(p.fromD3D)>(image, "NvCVImage_FromD3DFormat");

    p.cv_check(p.getVersion(&p.version), "NvVFX_GetVersion");

    int pixFmt = 0, compType = 0;
    unsigned char layout = 0;
    p.cv_check(p.fromD3D(DXGI_FORMAT_R8G8B8A8_UNORM, &pixFmt, &compType, &layout), "NvCVImage_FromD3DFormat");

    const size_t total = 2 + gpu.outputCount;
    for (size_t i = 0; i < total; ++i) {
        auto& buf = p.mapped[i];
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC hdesc{};
        hdesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE;
        hdesc.handle.win32.handle = gpu.exported[i].h;
        hdesc.size = gpu.allocated;
        hdesc.flags = CU_EXTERNAL_MEMORY_DEDICATED;
        p.cu_check(p.cuImportExternalMemory(&buf.memory, &hdesc), "cuImportExternalMemory");
        p.mappedCount = i + 1;

        CUDA_EXTERNAL_MEMORY_BUFFER_DESC bdesc{};
        bdesc.size = gpu.bytes;
        p.cu_check(p.cuExternalMemoryGetMappedBuffer(&buf.pointer, buf.memory, &bdesc), "cuExternalMemoryGetMappedBuffer");
        p.cv_check(p.imageCreate(1, 1, pixFmt, compType, layout, 0, 1, &buf.image), "NvCVImage_Create");
        p.imageDealloc(buf.image);
        p.cv_check(p.imageInit(buf.image, session.width, session.height, static_cast<int>(gpu.pitch()),
            reinterpret_cast<void*>(buf.pointer), pixFmt, compType, layout, 1), "NvCVImage_Init");
    }

    const unsigned mode = std::min<uint32_t>(session.quality, 2);
    p.cv_check(p.createEffect("VideoFrameGeneration", &p.effect), "NvVFX_CreateEffect");
    p.cv_check(p.setStream(p.effect, "CudaStream", p.stream), "NvVFX_SetCudaStream");
    p.cv_check(p.setU32(p.effect, "InputWidth", session.width), "InputWidth");
    p.cv_check(p.setU32(p.effect, "InputHeight", session.height), "InputHeight");
    p.cv_check(p.setU32(p.effect, "Mode", mode), "Mode");
    p.cv_check(p.setU32(p.effect, "AutomaticShotChangeDetectionEnabled", 1), "AutomaticShotChangeDetectionEnabled");
    p.cv_check(p.setImage(p.effect, "SrcImage0", p.mapped[0].image), "initial SrcImage0");
    p.cv_check(p.setImage(p.effect, "SrcImage1", p.mapped[1].image), "initial SrcImage1");
    p.cv_check(p.loadEffect(p.effect), "NvVFX_Load");
    p.cv_check(p.setImage(p.effect, "DstImage0", p.mapped[2].image), "initial DstImage0");
    p.cv_check(p.setU32(p.effect, "FrameMultiplier", p.multiplier), "FrameMultiplier");
}

NativeVfg::~NativeVfg() = default;

bool NativeVfg::generate_subframe(uint32_t subIndex, bool newPair, bool shotChange)
{
    auto& p = *impl_;
    if (!subIndex || subIndex >= p.multiplier) return false;
    Impl::CtxGuard ctx(p);
    if (newPair) {
        p.cv_check(p.setImage(p.effect, "SrcImage0", p.mapped[0].image), "SetImage SrcImage0");
        p.cv_check(p.setImage(p.effect, "SrcImage1", p.mapped[1].image), "SetImage SrcImage1");
        p.cv_check(p.setU32(p.effect, "ShotChange", shotChange ? 1u : 0u), "SetU32 ShotChange");
    }
    p.cv_check(p.setImage(p.effect, "DstImage0", p.mapped[1 + subIndex].image), "SetImage DstImage0");
    p.cv_check(p.setU32(p.effect, "FrameIndex", subIndex), "SetU32 FrameIndex");
    p.cv_check(p.runEffect(p.effect, 1), "NvVFX_Run");
    p.cu_check(p.cuStreamSynchronize(p.stream), "cuStreamSynchronize");
    return true;
}

bool NativeVfg::enqueue_subframes(bool shotChange)
{
    auto& p = *impl_;
    Impl::CtxGuard ctx(p);
    p.cv_check(p.setImage(p.effect, "SrcImage0", p.mapped[0].image), "SetImage SrcImage0");
    p.cv_check(p.setImage(p.effect, "SrcImage1", p.mapped[1].image), "SetImage SrcImage1");
    p.cv_check(p.setU32(p.effect, "ShotChange", shotChange ? 1u : 0u), "SetU32 ShotChange");
    for (uint32_t sub = 1; sub < p.multiplier; ++sub) {
        p.cv_check(p.setImage(p.effect, "DstImage0", p.mapped[1 + sub].image), "SetImage DstImage0");
        p.cv_check(p.setU32(p.effect, "FrameIndex", sub), "SetU32 FrameIndex");
        p.cv_check(p.runEffect(p.effect, 1), "NvVFX_Run");
        p.cu_check(p.cuEventRecord(p.subEvents[sub], p.stream), "cuEventRecord");
    }
    return true;
}

void NativeVfg::wait_subframe(uint32_t subIndex)
{
    auto& p = *impl_;
    if (!subIndex || subIndex >= p.multiplier || !p.subEvents[subIndex]) return;
    p.cu_check(p.cuEventSynchronize(p.subEvents[subIndex]), "cuEventSynchronize");
}

unsigned NativeVfg::sdk_version() const noexcept
{
    return impl_ ? impl_->version : 0;
}
} // namespace nr::fg::vfg
