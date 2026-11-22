// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): Based on PushFPS (GPL-3.0-or-later), modified 2026-11-22 for the OBS presenter.
#pragma once
#include "fg_backend.h"
#include "pushflow_policy.h"
#include "fg_vfg_session.h"
#include <cstdio>
#include <memory>

namespace nr::fg::vfg {
class Gpu;

struct PushFlowOpen {
    const Session& session;
    FILE* log;
    std::function<void()> beat;
    std::function<bool()> userClosing;
    unsigned expectedOutputs{}; // Optional caller constraint, checked before any inference.
    pushflow::PreviewMultiplier* preview{};
};

class PushFlowBackend final : public IFgBackend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool faulted_{}, runtimeFault_{};
    size_t outputCount_{};
    pushflow::ShaderPath shader_{pushflow::ShaderPath::Unpack};
public:
    PushFlowBackend();
    ~PushFlowBackend() override;
    bool open(const PushFlowOpen&, Gpu*);
    bool close();
    const char* name() const noexcept override { return "pushflow"; }
    unsigned version_tag() const noexcept override { return 0x01140100; }
    bool faulted() const noexcept override { return faulted_; }
    bool runtime_fault() const noexcept { return runtimeFault_; }
    void generation_fault() noexcept { if (impl_) faulted_ = runtimeFault_ = true; }
    size_t output_count() const noexcept { return outputCount_; }
    pushflow::ShaderPath shader_path() const noexcept { return shader_; }
    std::optional<FgPairTicket> submit_pair(const FgPairDesc&, unsigned, unsigned) override;
    bool subframe_ready(const FgPairTicket&, uint32_t) override;
    void resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource*, uint32_t) override;
    void synchronize() override;
};
} // namespace nr::fg::vfg
