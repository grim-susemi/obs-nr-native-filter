// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "xemfg_backend.h"
#include "xemfg_bank.h"
#include "xemfg_runtime.h"
#include <array>
#include <memory>
#include <string>

namespace nr::fg::vfg {
struct XeMfgBackend::Impl {
    const Extent size;
    const uint64_t generation;
    std::wstring path, xessRuntimeDir;
    FILE* const log;
    Gpu* const gpu;
    FILE* const submissionTrace;
    XeMfgRuntime runtime;
    std::array<std::unique_ptr<detail::Bank>, 2> banks;
    size_t bankCount{};
    uint32_t multiplier{}, maxGenerated{};
    uint64_t nextPairId{};
    bool teardownStarted{};

    explicit Impl(const XeMfgOpen& options);
    static bool consumer_complete(const detail::Bank& bank);
    detail::Bank* free_bank();
    void drain(detail::Bank& bank);
    bool create_feature(detail::Bank& bank);
    bool record_evaluate(detail::Bank& bank, ID3D12GraphicsCommandList* list, ID3D12Resource* color,
        ID3D12Resource* motion, ID3D12Resource* depth, unsigned sub, uint64_t serial, unsigned count,
        bool reset);
    bool evaluate_frame(detail::Bank& bank, ID3D12Resource* color, uint64_t serial, unsigned count,
        bool reset, bool shotCut);
};
} // namespace nr::fg::vfg
