// SPDX-License-Identifier: MIT
// Minimal declaration of MFG-Unlocker's XeMFG provider ABI, version 1.
//
// Adapted from dashdogy/MFG-Unlocker, source/native/xess/vulkan/mfg_xemfg_provider.h
// (MIT, Copyright (c) 2026 Michael Robles). The notice is reproduced in
// provenance/source-notices/MFG-Unlocker-MIT.txt. Only the exported names, the
// append-only field order and the ABI sizes are reproduced: this project never
// ships, patches or redistributes MFGUnlocker.dll or any Intel binary.
//
// Version 1 is append-only: a provider reports the sizeof of its own structure,
// so a caller must check `size` before reading any field that was appended.
#pragma once

#include <stdint.h>

struct NVSDK_NGX_Parameter;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

#define MFG_XEMFG_PROVIDER_API_VERSION 1
#define MFG_XEMFG_PROVIDER_GET_API "MfgXeMFGProviderGetApi"
#define MFG_XEMFG_PROVIDER_SHIM_NAME L"mfgunlocker_xemfg.dll"

typedef struct MfgXeMFGProviderApi {
    uint32_t size;    // sizeof(MfgXeMFGProviderApi) of the running provider
    uint32_t version; // MFG_XEMFG_PROVIDER_API_VERSION

    // vkprov.1: the Vulkan entries at their fixed version-1 offsets. The D3D12
    // route never calls them; the pointer types stay opaque so this header does
    // not require the Vulkan SDK. On the Windows x64 ABI every one of these
    // arguments is a pointer, so the layout and calling convention are exact.
    int32_t (*VulkanInit)(void* instance, void* physical, void* device, void* get_instance_proc_addr,
        void* get_device_proc_addr);
    int32_t (*VulkanCreateFeature)(void* device, void* command, struct NVSDK_NGX_Parameter* parameters,
        void** feature);
    int32_t (*VulkanEvaluate)(void* command, void* feature, struct NVSDK_NGX_Parameter* parameters);
    int32_t (*ReleaseFeature)(void* feature); // a Vulkan feature
    int32_t (*Shutdown)(void);                // Vulkan
    // Generated frames per real frame the provider can make (DLSSG.MultiFrameCountMax);
    // 0 once Refusal is set.
    uint32_t (*MaxGeneratedFrames)(void);

    // ngxprov.1 (appended). Nonzero when the provider cannot run in this process;
    // `reason` is NUL-terminated and cut to `capacity`. Lock-free and loading
    // nothing, so a shim can ask before it admits the module.
    uint32_t (*Refusal)(char* reason, uint32_t capacity);
    int32_t (*D3D12Init)(struct ID3D12Device* device);
    int32_t (*D3D12CreateFeature)(struct ID3D12GraphicsCommandList* list,
        struct NVSDK_NGX_Parameter* parameters, void** feature);
    // One generated frame per call, DLSSG.MultiFrameIndex 1..DLSSG.MultiFrameCount
    // copied into DLSSG.OutputInterpolated.
    int32_t (*D3D12Evaluate)(struct ID3D12GraphicsCommandList* list, void* feature,
        struct NVSDK_NGX_Parameter* parameters);
    int32_t (*D3D12ReleaseFeature)(void* feature);
    int32_t (*D3D12Shutdown)(void);
} MfgXeMFGProviderApi;

// vkprov.1-.4 providers end after MaxGeneratedFrames.
#define MFG_XEMFG_PROVIDER_API_SIZE_VULKAN (2u * sizeof(uint32_t) + 6u * sizeof(void*))
// The full version-1 table this D3D12 caller requires.
#define MFG_XEMFG_PROVIDER_API_SIZE_D3D12 (2u * sizeof(uint32_t) + 12u * sizeof(void*))

typedef const MfgXeMFGProviderApi* (*PFN_MfgXeMFGProviderGetApi)(uint32_t version);

#if defined(__cplusplus)
static_assert(MFG_XEMFG_PROVIDER_API_SIZE_D3D12 == sizeof(MfgXeMFGProviderApi),
    "MfgXeMFGProviderApi must match MFG-Unlocker's version-1 append-only layout");
#endif
