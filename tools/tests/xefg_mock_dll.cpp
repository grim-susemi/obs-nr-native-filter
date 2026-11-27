// SPDX-License-Identifier: GPL-3.0-only
// Test fixture for the official XeSS 3.0.2 ABI. Two copies are built with
// different export macros and output names (libxess_fg.dll, libxell.dll). Every
// entry point appends a line to OBS_NR_TEST_XESS_CALL_LOG so the presenter test
// can assert 2D constants, tag extents, and destroy order from outside the
// child process. The real swapchain is created through the factory the
// presenter passes in, exactly like the official library creates its proxy.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <utility>

namespace {
void log_line(const char* format, ...)
{
    wchar_t path[1024]{};
    const DWORD length = GetEnvironmentVariableW(L"OBS_NR_TEST_XESS_CALL_LOG", path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a") != 0 || !file) return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}
}

#ifdef XEFG_MOCK_LIBRARY_FG
#include <dxgi1_5.h>
#include <wrl/client.h>
#include "xess_fg/xefg_swapchain_d3d12.h"

namespace {
xefg_swapchain_handle_t fg_context()
{
    return reinterpret_cast<xefg_swapchain_handle_t>(static_cast<uintptr_t>(0x58454746));
}
bool force_latency_unsupported()
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"OBS_NR_TEST_XESS_FORCE_LATENCY_UNSUPPORTED", value,
               ARRAYSIZE(value)) == 1 && value[0] == L'1';
}
Microsoft::WRL::ComPtr<IDXGISwapChain1> fg_swap;
bool fg_latency_retried;
}

extern "C" {
XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainGetVersion(xefg_swapchain_version_t* version)
{
    if (version) *version = xefg_swapchain_version_t{3, 0, 2, 0};
    log_line("xefg_get_version version=3.0.2");
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainGetProperties(xefg_swapchain_handle_t,
    xefg_swapchain_properties_t* properties)
{
    if (!properties) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    *properties = {};
    properties->maxSupportedInterpolations = 3;
    log_line("xefg_get_properties max_interpolations=3");
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainDestroy(xefg_swapchain_handle_t)
{
    log_line("xefg_destroy");
    fg_swap.Reset();
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetEnabled(xefg_swapchain_handle_t, uint32_t enable)
{
    if (force_latency_unsupported() && !fg_latency_retried) {
        fg_latency_retried = true;
        log_line("xefg_enabled latency_unsupported");
        return XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED;
    }
    log_line("xefg_enabled enabled=%u", enable);
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetLatencyReduction(xefg_swapchain_handle_t, void* context)
{
    log_line("xefg_latency_reduction context=%p", context);
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainTagFrameConstants(xefg_swapchain_handle_t,
    uint32_t presentId, const xefg_swapchain_frame_constant_data_t* constants)
{
    if (!constants) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    log_line("xefg_tag_constants present_id=%u reset=%u view0=%.6f proj0=%.6f proj5=%.6f "
             "proj10=%.6f proj11=%.6f jitter=%.6f,%.6f mv_scale=%.6f,%.6f render_ms=%.3f",
        presentId, constants->resetHistory, constants->viewMatrix[0], constants->projectionMatrix[0],
        constants->projectionMatrix[5], constants->projectionMatrix[10], constants->projectionMatrix[11],
        constants->jitterOffsetX, constants->jitterOffsetY, constants->motionVectorScaleX,
        constants->motionVectorScaleY, constants->frameRenderTime);
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainSetPresentId(xefg_swapchain_handle_t, uint32_t presentId)
{
    log_line("xefg_present_id present_id=%u", presentId);
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainGetLastPresentStatus(xefg_swapchain_handle_t,
    xefg_swapchain_present_status_t* status)
{
    if (!status) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    *status = {};
    status->framesPresented = 2;
    status->frameGenResult = XEFG_SWAPCHAIN_RESULT_SUCCESS;
    status->isFrameGenEnabled = 1;
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12CreateContext(ID3D12Device* device,
    xefg_swapchain_handle_t* context)
{
    if (!device || !context) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    *context = fg_context();
    log_line("xefg_create_context device=%p", static_cast<void*>(device));
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12InitFromSwapChainDesc(
    xefg_swapchain_handle_t, HWND window, const DXGI_SWAP_CHAIN_DESC1* description,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, ID3D12CommandQueue* queue,
    IDXGIFactory2* factory, const xefg_swapchain_d3d12_init_params_t* parameters)
{
    if (!window || !description || !queue || !factory || !parameters)
        return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    log_line("xefg_init hwnd=%p size=%ux%u format=%u buffers=%u max_interpolations=%u ui_mode=%u",
        static_cast<void*>(window), description->Width, description->Height, description->Format,
        description->BufferCount, parameters->maxInterpolatedFrames, static_cast<unsigned>(parameters->uiMode));
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap;
    const HRESULT hr = factory->CreateSwapChainForHwnd(queue, window, description, fullscreen, nullptr, &swap);
    if (FAILED(hr)) {
        log_line("xefg_init_failed hr=0x%08lX", static_cast<unsigned long>(hr));
        return XEFG_SWAPCHAIN_RESULT_ERROR_HRESULT_FAILURE;
    }
    fg_swap = std::move(swap);
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12GetSwapChainPtr(xefg_swapchain_handle_t,
    REFIID iid, void** swapchain)
{
    if (!swapchain) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    if (!fg_swap) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_CONTEXT;
    const HRESULT hr = fg_swap->QueryInterface(iid, swapchain);
    log_line("xefg_get_swapchain_ptr hr=0x%08lX", static_cast<unsigned long>(hr));
    return SUCCEEDED(hr) ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE;
}

XEFG_SWAPCHAIN_API xefg_swapchain_result_t xefgSwapChainD3D12TagFrameResource(xefg_swapchain_handle_t,
    ID3D12CommandList* commandList, uint32_t presentId, const xefg_swapchain_d3d12_resource_data_t* data)
{
    if (!data || !data->pResource) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    const auto description = data->pResource->GetDesc();
    log_line("xefg_tag_resource present_id=%u type=%d validity=%d state=%d w=%u h=%u format=%d cmd=%p",
        presentId, static_cast<int>(data->type), static_cast<int>(data->validity),
        static_cast<int>(data->incomingState), data->resourceSize.x, data->resourceSize.y,
        static_cast<int>(description.Format), static_cast<void*>(commandList));
    return XEFG_SWAPCHAIN_RESULT_SUCCESS;
}
}
#endif

#ifdef XEFG_MOCK_LIBRARY_XELL
#include "xell/xell_d3d12.h"

namespace {
xell_context_handle_t xell_context()
{
    return reinterpret_cast<xell_context_handle_t>(static_cast<uintptr_t>(0x58454C4C));
}
}

extern "C" {
XELL_EXPORT xell_result_t xellD3D12CreateContext(ID3D12Device* device, xell_context_handle_t* context)
{
    if (!device || !context) return XELL_RESULT_ERROR_INVALID_ARGUMENT;
    *context = xell_context();
    log_line("xell_create device=%p", static_cast<void*>(device));
    return XELL_RESULT_SUCCESS;
}

XELL_EXPORT xell_result_t xellSetSleepMode(xell_context_handle_t, const xell_sleep_params_t* parameters)
{
    log_line("xell_sleep_mode low_latency=%u", parameters ? parameters->bLowLatencyMode : 0u);
    return XELL_RESULT_SUCCESS;
}

XELL_EXPORT xell_result_t xellAddMarkerData(xell_context_handle_t, uint32_t frameId,
    xell_latency_marker_type_t marker)
{
    log_line("xell_marker frame_id=%u marker=%d", frameId, static_cast<int>(marker));
    return XELL_RESULT_SUCCESS;
}

XELL_EXPORT xell_result_t xellSleep(xell_context_handle_t, uint32_t frameId)
{
    log_line("xell_sleep frame_id=%u", frameId);
    return XELL_RESULT_SUCCESS;
}

XELL_EXPORT xell_result_t xellDestroyContext(xell_context_handle_t)
{
    log_line("xell_destroy");
    return XELL_RESULT_SUCCESS;
}

XELL_EXPORT xell_result_t xellGetVersion(xell_version_t* version)
{
    if (version) *version = xell_version_t{1, 3, 2, 0};
    log_line("xell_get_version version=1.3.2");
    return XELL_RESULT_SUCCESS;
}
}
#endif
