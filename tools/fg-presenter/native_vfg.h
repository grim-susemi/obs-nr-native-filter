// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "vfg_gpu.h"
#include <filesystem>
#include <memory>

namespace nr::fg::vfg {
class NativeVfg final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeVfg(const std::filesystem::path& runtimeDir, const Gpu& gpu, const Session& session);
    ~NativeVfg();
    NativeVfg(const NativeVfg&) = delete;
    NativeVfg& operator=(const NativeVfg&) = delete;
    // stageFence is the signal already queued by Gpu::try_stage. CUDA waits on
    // it before reading the pair and records each output's completion in order.
    bool enqueue_subframes(uint64_t stageFence, bool shotChange);
    void wait_subframe(uint32_t subIndex);
    bool subframe_ready(uint32_t subIndex);
    void synchronize();
    unsigned sdk_version() const noexcept;
};
} // namespace nr::fg::vfg
