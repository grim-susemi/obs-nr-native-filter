// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_backend.h"
#include "vfg_gpu.h"
#include <filesystem>
#include <memory>

namespace nr::fg::vfg {
class NativeVfg final : public IFgBackend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeVfg(const std::filesystem::path& runtimeDir, Gpu& gpu, const Session& session,
        bool gpuShotChange = false, const std::function<void()>& beat = {});
    ~NativeVfg() override;
    NativeVfg(const NativeVfg&) = delete;
    NativeVfg& operator=(const NativeVfg&) = delete;
    const char* name() const noexcept override { return "nvidia-vfg-native"; }
    unsigned version_tag() const noexcept override;
    bool faulted() const noexcept override;
    std::optional<FgPairTicket> submit_pair(const FgPairDesc&, unsigned prevIndex, unsigned currIndex) override;
    bool subframe_ready(const FgPairTicket&, uint32_t sub) override;
    void resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource*, uint32_t sub) override;
    void synchronize() override;
};
} // namespace nr::fg::vfg
