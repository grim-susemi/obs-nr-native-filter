// SPDX-License-Identifier: GPL-3.0-only

namespace {
// An ordinary initialized object keeps the linked DLL above the validator's 64 KiB minimum.
const volatile unsigned char padding[65536]{42};
}

extern "C" {
__declspec(dllexport) int NVSDK_NGX_D3D12_Init_Ext() { return padding[0] + padding[65535]; }
__declspec(dllexport) int NVSDK_NGX_D3D12_PopulateParameters_Impl() { return 42; }
__declspec(dllexport) int NVSDK_NGX_D3D12_CreateFeature() { return 42; }
__declspec(dllexport) int NVSDK_NGX_D3D12_EvaluateFeature() { return 42; }
__declspec(dllexport) int NVSDK_NGX_D3D12_ReleaseFeature() { return 42; }
__declspec(dllexport) int NVSDK_NGX_D3D12_Shutdown1() { return 42; }
}
