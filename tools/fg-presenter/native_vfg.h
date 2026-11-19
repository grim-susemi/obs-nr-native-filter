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
    // Binds the current (A, B) input pair once, then runs FrameIndex=sub (1..multiplier-1)
    // and synchronizes the CUDA stream so gpu.exchange[1 + sub] is ready for D3D12 present.
    bool generate_subframe(uint32_t subIndex, bool newPair, bool shotChange);
    bool enqueue_subframes(bool shotChange);
    void wait_subframe(uint32_t subIndex);
    unsigned sdk_version() const noexcept;
};
} // namespace nr::fg::vfg
