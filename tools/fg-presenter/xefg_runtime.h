// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Official Intel XeSS 3.0.2 XeFG + XeLL runtime admission.
//
// The two libraries are never linked, copied, or patched. They are loaded from
// the session-provided directory (or <exe>/xess-fg), admitted only when the
// embedded Authenticode signer is Intel Corporation and the SHA-256 matches the
// official XeSS_SDK_3.0.2.zip binaries, and every export this presenter calls is
// resolved up front so a partial runtime can never start a generation.
#include "xell/xell_d3d12.h"
#include "xess_fg/xefg_swapchain_d3d12.h"
#include "xess_fg/xefg_swapchain_debug.h"
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace nr::fg::vfg {
// Pinned official XeSS_SDK_3.0.2.zip binaries: libxess_fg.dll 1.3.1.78 and
// libxell.dll 1.3.2.10. The hashes, not a mutable file version, are the identity.
inline constexpr std::string_view xefg_pinned_sha256 =
    "ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27";
inline constexpr std::string_view xell_pinned_sha256 =
    "d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7";

enum class XeFgLoadStatus : uint32_t {
    Ok = 0,
    MissingLibrary,
    UntrustedPath,
    SignatureRejected,
    HashMismatch,
    ExportMissing,
    LoadFailed,
};
const char* xefg_load_status_name(XeFgLoadStatus status) noexcept;

// Every entry point the presenter uses. A runtime is only usable when all of
// them are non-null; the optional versions are telemetry only.
struct XeFgExports {
    xefg_swapchain_result_t (*get_version)(xefg_swapchain_version_t*) {};
    xefg_swapchain_result_t (*get_properties)(xefg_swapchain_handle_t, xefg_swapchain_properties_t*) {};
    xefg_swapchain_result_t (*destroy)(xefg_swapchain_handle_t) {};
    xefg_swapchain_result_t (*set_enabled)(xefg_swapchain_handle_t, uint32_t) {};
    xefg_swapchain_result_t (*set_latency_reduction)(xefg_swapchain_handle_t, void*) {};
    xefg_swapchain_result_t (*tag_frame_constants)(
        xefg_swapchain_handle_t, uint32_t, const xefg_swapchain_frame_constant_data_t*) {};
    xefg_swapchain_result_t (*set_present_id)(xefg_swapchain_handle_t, uint32_t) {};
    xefg_swapchain_result_t (*get_last_present_status)(
        xefg_swapchain_handle_t, xefg_swapchain_present_status_t*) {};
    xefg_swapchain_result_t (*d3d12_create_context)(ID3D12Device*, xefg_swapchain_handle_t*) {};
    xefg_swapchain_result_t (*d3d12_init_from_desc)(xefg_swapchain_handle_t, HWND,
        const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*,
        IDXGIFactory2*, const xefg_swapchain_d3d12_init_params_t*) {};
    xefg_swapchain_result_t (*d3d12_get_swapchain_ptr)(
        xefg_swapchain_handle_t, REFIID, void**) {};
    xefg_swapchain_result_t (*d3d12_tag_resource)(
        xefg_swapchain_handle_t, ID3D12CommandList*, uint32_t, const xefg_swapchain_d3d12_resource_data_t*) {};
    xell_result_t (*xell_get_version)(xell_version_t*) {};
    xell_result_t (*xell_create)(ID3D12Device*, xell_context_handle_t*) {};
    xell_result_t (*xell_set_sleep_mode)(xell_context_handle_t, const xell_sleep_params_t*) {};
    xell_result_t (*xell_add_marker)(xell_context_handle_t, uint32_t, xell_latency_marker_type_t) {};
    xell_result_t (*xell_sleep)(xell_context_handle_t, uint32_t) {};
    xell_result_t (*xell_destroy)(xell_context_handle_t) {};
    // The rest of the public 3.0.2 table. Every entry is resolved when present so
    // the admitted image is the whole documented ABI; the 2x path does not call
    // these and does not require them to exist.
    xefg_swapchain_result_t (*set_ui_composition_state)(xefg_swapchain_handle_t,
        xefg_swapchain_ui_composition_state_t) {};
    xefg_swapchain_result_t (*set_num_interpolated_frames)(xefg_swapchain_handle_t, uint32_t) {};
    xefg_swapchain_result_t (*set_scene_change_threshold)(xefg_swapchain_handle_t, float) {};
    xefg_swapchain_result_t (*set_logging_callback)(xefg_swapchain_handle_t,
        xefg_swapchain_logging_level_t, xefg_swapchain_app_log_callback_t, void*) {};
    xefg_swapchain_result_t (*d3d12_build_pipelines)(xefg_swapchain_handle_t,
        ID3D12PipelineLibrary*, uint8_t, uint32_t) {};
    xefg_swapchain_result_t (*d3d12_set_descriptor_heap)(xefg_swapchain_handle_t,
        ID3D12DescriptorHeap*, uint32_t) {};
    xefg_swapchain_result_t (*d3d12_get_properties)(xefg_swapchain_handle_t,
        const xefg_swapchain_d3d12_init_params_t*, uint32_t, uint32_t, DXGI_FORMAT,
        xefg_swapchain_properties_t*) {};
    xefg_swapchain_result_t (*d3d12_get_initialization_parameters)(xefg_swapchain_handle_t,
        xefg_swapchain_d3d12_init_params_t*) {};
    xefg_swapchain_result_t (*d3d12_update_external_heap)(xefg_swapchain_handle_t, ID3D12Heap*,
        uint64_t, ID3D12Heap*, uint64_t) {};
    xefg_swapchain_result_t (*enable_debug_feature)(xefg_swapchain_handle_t,
        xefg_swapchain_debug_feature_t, uint32_t, void*) {};
    xell_result_t (*xell_get_sleep_mode)(xell_context_handle_t, xell_sleep_params_t*) {};
    xell_result_t (*xell_set_logging_callback)(xell_context_handle_t, xell_logging_level_t,
        xell_app_log_callback_t) {};
    xell_result_t (*xell_get_frames_reports)(xell_context_handle_t, xell_frame_report_t*) {};
};

class XeFgRuntime final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    XeFgRuntime();
    ~XeFgRuntime();
    XeFgRuntime(const XeFgRuntime&) = delete;
    XeFgRuntime& operator=(const XeFgRuntime&) = delete;
    // Idempotent: a second open() releases the previous modules first.
    XeFgLoadStatus open(std::wstring_view directory, FILE* log);
    void close() noexcept;
    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] const XeFgExports& api() const noexcept;
    [[nodiscard]] std::wstring_view directory() const noexcept;
    // True when this build admitted unsigned test fixtures through the
    // NR_FG_TEST_MOCK_XESS environment pins. Never contains a success path in a
    // production build.
    [[nodiscard]] bool test_fixture() const noexcept;
    // Static literal naming the rejected gate ("signature", "hash", "export").
    [[nodiscard]] const char* detail() const noexcept;
    [[nodiscard]] xefg_swapchain_version_t swapchain_version() const noexcept;
    [[nodiscard]] xell_version_t xell_version() const noexcept;
};
} // namespace nr::fg::vfg
