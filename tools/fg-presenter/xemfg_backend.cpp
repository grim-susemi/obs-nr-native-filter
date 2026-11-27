// SPDX-License-Identifier: GPL-3.0-only
// IFgBackend contract for the opt-in XeMFG transport: admission, per-bank
// ownership, pair retirement and teardown. Evaluation and parameter population
// live in xemfg_evaluate.cpp; provider ABI/SEH isolation in xemfg_runtime.cpp.
#include "xemfg_impl.h"
#include <algorithm>
#include <exception>

namespace nr::fg::vfg {
XeMfgBackend::Impl::Impl(const XeMfgOpen& options)
    : size(options.size), generation(options.generation), path(options.path),
      xessRuntimeDir(options.xessRuntimeDir), log(options.log), gpu(options.gpu),
      submissionTrace(options.submissionTrace)
{}

XeMfgBackend::XeMfgBackend() = default;

XeMfgBackend::~XeMfgBackend()
{
    try {
        if (close()) return;
    } catch (...) {
        // An unconfirmed drain cannot release external ownership during unwind.
    }
    (void)impl_.release();
}

bool XeMfgBackend::open(const XeMfgOpen& args)
{
    faulted_ = true;
    if (impl_) return false;
    if (!args.device || !args.queue || !args.log || !args.gpu) {
        if (args.log) fprintf(args.log, "event=xemfg_runtime_fault reason=arguments\n");
        return false;
    }
    if (args.size.width < 640 || args.size.width > 7680 || args.size.height < 360 || args.size.height > 4320) {
        if (args.log) fprintf(args.log, "event=xemfg_runtime_fault reason=unsupported_size width=%u height=%u\n",
            args.size.width, args.size.height);
        return false;
    }
    impl_ = std::make_unique<Impl>(args);
    auto& s = *impl_;
    try {
        if (!s.runtime.open(args.path, args.optIn, args.xessRuntimeDir, args.device, args.log)) return false;
        s.maxGenerated = s.runtime.max_generated();
        s.multiplier = std::clamp(args.multiplier, 2u, s.maxGenerated + 1);
        s.bankCount = s.multiplier >= 4 ? 1 : 2;
        for (size_t i = 0; i < s.bankCount; ++i) {
            s.banks[i] = std::make_unique<detail::Bank>(args.device, args.queue, args, i);
            if (!s.create_feature(*s.banks[i])) {
                if (args.log) fprintf(args.log, "event=xemfg_runtime_fault reason=feature_create bank=%zu\n", i);
                return false;
            }
        }
        if (args.log) {
            const auto companions = s.runtime.companion_directory();
            fprintf(args.log,
                "event=xemfg_initialized multiplier=%u max_generated=%u banks=%zu companions=%.*ls\n",
                s.multiplier, s.maxGenerated, s.bankCount, static_cast<int>(companions.size()), companions.data());
        }
        faulted_ = false;
        return true;
    } catch (const std::exception& error) {
        if (args.log) fprintf(args.log, "event=xemfg_runtime_fault reason=initialization message=%s\n", error.what());
        return false;
    }
}

bool XeMfgBackend::close_impl()
{
    if (!impl_) return true;
    auto& s = *impl_;
    if (s.teardownStarted) return false;
    s.teardownStarted = true;
    faulted_ = true;
    try {
        for (auto& bank : s.banks) if (bank) s.drain(*bank);
        for (auto& bank : s.banks) if (bank) {
            bank->bgra.reset(); // Collect releases guide registrations before their allocations.
            if (bank->guides && !bank->guides->close()) {
                if (s.log) fprintf(s.log, "event=xemfg_runtime_fault reason=guide_close_unconfirmed\n");
                return false;
            }
            if (bank->featureCreated) {
                if (!s.runtime.release_feature(bank->feature)) return false;
                bank->feature = nullptr;
                bank->featureCreated = false;
            }
        }
        if (!s.runtime.close()) return false;
        impl_.reset();
        return true;
    } catch (const std::exception& error) {
        if (s.log) fprintf(s.log, "event=xemfg_runtime_fault reason=teardown message=%s\n", error.what());
        return false;
    }
}

bool XeMfgBackend::close() noexcept
{
    try {
        return close_impl();
    } catch (...) {
        return false;
    }
}

uint32_t XeMfgBackend::multiplier_limit() const noexcept
{
    return faulted_ || !impl_ ? 1u : std::min(max_multiplier, impl_->maxGenerated + 1);
}

size_t XeMfgBackend::max_in_flight_pairs() const noexcept
{
    return !impl_ || impl_->multiplier >= 4 ? 1 : 2;
}

std::optional<FgPairTicket> XeMfgBackend::submit_pair(const FgPairDesc& pair, unsigned prevIndex, unsigned currIndex)
{
    if (faulted_ || !impl_ || pair.discontinuity || pair.effectiveMultiplier < 2) return {};
    auto& s = *impl_;
    if (flow_routing(pair.flow) == FlowRouting::NrUnavailable) return {};
    auto* bank = s.free_bank();
    if (!bank) return {};
    const unsigned count = std::min(s.maxGenerated, pair.effectiveMultiplier - 1);
    if (!count) return {};
    try {
        bank->ticket.reset();
        s.drain(*bank);
        bank->commands->recycle();
        bank->guides->configure(pair.flow);
        if (pair.onStageQueued) pair.onStageQueued();
        const auto stageStart = clock_ns();
        bool ok = true;
        if (!bank->previous || bank->lastSerial != pair.prevSerial)
            ok = s.evaluate_frame(*bank, pair.prevTex, pair.prevSerial, 1, true, false);
        if (ok) ok = s.evaluate_frame(*bank, pair.currTex, pair.currSerial, count, false,
            pair.shotCut || pair.flow.reset);
        if (s.gpu) s.gpu->mark_external_inputs_read(prevIndex, currIndex);
        if (!ok) {
            faulted_ = true;
            return {};
        }
        if (pair.onEnqueued) pair.onEnqueued(clock_ns() - stageStart);
    } catch (const std::exception& error) {
        if (s.log) fprintf(s.log, "event=xemfg_runtime_fault reason=dispatch message=%s\n", error.what());
        faulted_ = true;
        if (s.gpu) s.gpu->mark_external_inputs_read(prevIndex, currIndex);
        return {};
    }
    bank->ticket.emplace(FgPairTicket{++s.nextPairId, prevIndex, currIndex, count + 1, count,
        static_cast<uint32_t>(bank->index)});
    bank->retired = false;
    bank->consumeFence.Reset();
    bank->consumeValue = 0;
    return bank->ticket;
}

bool XeMfgBackend::pair_complete(const FgPairTicket& ticket) const
{
    if (!impl_) return false;
    for (auto& bank : impl_->banks)
        if (bank && bank->ticket && *bank->ticket == ticket) return bank->commands->complete(bank->fence);
    return false;
}

bool XeMfgBackend::subframe_ready(const FgPairTicket& ticket, uint32_t)
{
    return !faulted_ && pair_complete(ticket);
}

void XeMfgBackend::resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource*, uint32_t)
{
    // D3D12Evaluate already wrote every subframe into the exact Gpu::syntheticTex
    // slot this ticket/sub names, and its list returned the texture to COMMON.
}

void XeMfgBackend::retire_pair(const FgPairTicket& ticket, ID3D12Fence* consumeFence, uint64_t consumeValue) noexcept
{
    if (!impl_) return;
    for (auto& bank : impl_->banks)
        if (bank && bank->ticket && *bank->ticket == ticket) {
            bank->consumeFence = consumeFence;
            bank->consumeValue = consumeValue;
            bank->retired = true;
            return;
        }
}

void XeMfgBackend::synchronize()
{
    if (!impl_) return;
    for (auto& bank : impl_->banks) if (bank) impl_->drain(*bank);
}

SharedGuideStats XeMfgBackend::guide_stats() const noexcept
{
    SharedGuideStats total{};
    if (!impl_) return total;
    for (auto& bank : impl_->banks) {
        if (!bank || !bank->guides) continue;
        const auto stats = bank->guides->stats();
        total.ofaExecutions += stats.ofaExecutions;
        total.sharedHits += stats.sharedHits;
        total.sharedResets += stats.sharedResets;
        total.sharedUnavailable += stats.sharedUnavailable;
    }
    return total;
}
} // namespace nr::fg::vfg
