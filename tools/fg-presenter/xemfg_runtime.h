// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Admission and SEH-isolated calls for a user-supplied MFGUnlocker.dll.
// Mirrors anr_runtime: every provider entry point runs through protected_call,
// a fault quarantines the image instead of unloading it under a live thread, and
// a failed D3D12Shutdown never reaches FreeLibrary.
#include "mfg_xemfg_provider.h"
#include "ngx_parameters.h"
#include "fg/xemfg_dll_validator.h"
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace nr::fg::vfg {
class XeMfgRuntime {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    XeMfgRuntime();
    ~XeMfgRuntime();
    XeMfgRuntime(const XeMfgRuntime&) = delete;
    XeMfgRuntime& operator=(const XeMfgRuntime&) = delete;
    // Validates the provider and its Intel companions, then loads and initializes
    // it. False means every provider object was released (or the image was
    // quarantined after a fault and deliberately left loaded).
    bool open(std::wstring_view path, bool optIn, std::wstring_view xessRuntimeDir,
        ID3D12Device* device, FILE* log);
    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] bool quarantined() const noexcept;
    [[nodiscard]] const MfgXeMFGProviderApi& api() const noexcept;
    [[nodiscard]] uint32_t max_generated() const noexcept;
    [[nodiscard]] std::wstring_view companion_directory() const noexcept;
    [[nodiscard]] std::string_view sha256() const noexcept;
    // The caller must release every feature before close().
    bool create_feature(ID3D12GraphicsCommandList*, NgxParameters&, void*& feature);
    bool evaluate(ID3D12GraphicsCommandList*, void* feature, NgxParameters&);
    bool release_feature(void* feature);
    // No-op after a quarantine: the image stays mapped for the process lifetime.
    bool close() noexcept;
};
} // namespace nr::fg::vfg
