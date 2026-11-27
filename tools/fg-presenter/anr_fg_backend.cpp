// SPDX-License-Identifier: GPL-3.0-only
// Adapted from DosukaSOL and the PushFPS contributors, PushFPS 4e5c0491:
// 2026-11-22: connect ANR dispatch to OBS presenter fencing and OFA guides.
#include "anr_fg_backend.h"
#include "anr_commands.h"
#include "anr_d3d.h"
#include <algorithm>
#include <intrin.h>

namespace nr::fg::vfg {
struct AnrFgBackend::Impl {
    const Extent size;
    const uint64_t generation;
    Gpu* const gpu;
    FILE* const log;
    std::unique_ptr<AnrFeature> feature;
    std::unique_ptr<IAnrGuideProvider> guides;
    std::unique_ptr<AnrBgra> bgra;
    std::unique_ptr<AnrCommands> commands;
    std::unique_ptr<AnrDispatch> dispatch;
    std::optional<FlowImage> previous;
    std::optional<JobKey> pending;
    std::optional<FgPairTicket> ticket;
    ComPtr<ID3D12Fence> consumeFence;
    uint64_t consumeValue{};
    bool retired{true};
    std::array<std::optional<Output>, max_multiplier - 1> selection;
    unsigned generated{}, slot{};
    uint64_t lastSerial{}, sequence{}, epoch{}, pairId{};
    bool guidesUncertain{};
    bool teardownStarted{};

    explicit Impl(const AnrOpen& opts) : size(opts.size), generation(opts.generation),
        gpu(opts.gpu), log(opts.log), feature(std::make_unique<AnrFeature>(opts.log)) {}

    void drain()
    {
        if (!commands) return;
        commands->wait();
        if (guidesUncertain) check(E_FAIL, "ANR guide ownership unconfirmed");
        if (!pending) return;
        if (!guides->collect(*pending, commands->last_point())) check(E_FAIL, "ANR guide collect");
        pending.reset();
    }

    bool consumer_complete() const
    {
        if (!consumeFence) return true;
        const auto completed = consumeFence->GetCompletedValue();
        return completed != UINT64_MAX && completed >= consumeValue;
    }

    bool evaluate_frame(ID3D12Resource* color, uint64_t serial, unsigned count, bool reset)
    {
        if (reset) ++epoch;
        const auto dense = ++sequence;
        const JobKey key{{generation, epoch, dense}, epoch, generation, dense};
        auto* list = commands->begin();
        FlowProblem problem = FlowProblem::None;
        const FlowImage imported{key, nullptr, NV_OF_FENCE_POINT{}};
        if (guides->shared_path(list, imported, reset, problem)) {
            // NR owns this pair: imported motion is consumed as-is (or scaled)
            // and local BGRA + NVOF never run. A failed import withholds the
            // pair instead of returning a synthesized frame with no motion.
            const auto* result = guides->result();
            if (problem != FlowProblem::None || !result || result->key != key) {
                if (log) fprintf(log, "event=anr_runtime_fault reason=shared_flow status=%u\n", static_cast<unsigned>(problem));
                return false;
            }
            pending.emplace(key);
            commands->enqueue_wait(result->complete);
            guidesUncertain = false;
            previous.reset();
            for (unsigned sub = 1; sub <= count; ++sub) {
                if (!dispatch->evaluate(*commands, *feature,
                        {color, result->motion, result->depth, nullptr, nullptr, serial, count, sub, reset}))
                    return false;
            }
            lastSerial = serial;
            return true;
        }
        bgra->record(list, color, slot);
        const auto converted = commands->submit(list);
        const FlowImage image{key, bgra->get(slot), converted};
        const FlowInput input{image, reset ? nullptr : &*previous, size, {0, 0, size.width, size.height}, reset};
        guidesUncertain = true;
        const auto status = guides->submit(input);
        const auto* result = guides->result();
        if (status != FlowProblem::None || !result || result->key != key) {
            if (log) fprintf(log, "event=anr_runtime_fault reason=guides status=%u\n", static_cast<unsigned>(status));
            return false;
        }
        pending.emplace(key);
        commands->enqueue_wait(result->complete);
        guidesUncertain = false;
        for (unsigned sub = 1; sub <= count; ++sub) {
            if (!dispatch->evaluate(*commands, *feature,
                    {color, result->motion, result->depth, nullptr, nullptr, serial, count, sub, reset}))
                return false;
        }
        previous.emplace(image);
        slot ^= 1u;
        lastSerial = serial;
        return true;
    }
};

AnrFgBackend::AnrFgBackend() = default;

AnrFgBackend::~AnrFgBackend()
{
    try {
        if (close()) return;
    } catch (...) {
        // An unconfirmed drain cannot release external ownership during unwind.
    }
    (void)impl_.release();
}

bool AnrFgBackend::close() noexcept
{
    try {
        return close_impl();
    } catch (...) {
        return false;
    }
}

bool AnrFgBackend::close_impl()
{
    if (!impl_) return true;
    auto& s = *impl_;
    if (s.teardownStarted) return false;
    s.teardownStarted = true;
    faulted_ = true;
    s.drain();
    s.bgra.reset(); // Collect releases the guide registrations before their allocations.
    if (s.guides && !s.guides->close()) {
        if (s.log) fprintf(s.log, "event=anr_runtime_fault reason=guide_close_unconfirmed\n");
        return false;
    }
    if (!s.feature->close()) return false;
    impl_.reset();
    return true;
}

bool AnrFgBackend::open(const AnrOpen& args, std::unique_ptr<IAnrGuideProvider> guides)
{
    faulted_ = true;
    if (impl_) return false;
    if (args.size.width < 640 || args.size.width > 7680 || args.size.height < 360 || args.size.height > 4320) {
        if (args.log) fprintf(args.log, "event=anr_runtime_fault reason=unsupported_size width=%u height=%u\n",
            args.size.width, args.size.height);
        return false;
    }
    impl_ = std::make_unique<Impl>(args);
    auto& s = *impl_;
    try {
        s.generated = std::min(s.feature->open(args.device, args.path, args.optIn), max_multiplier - 1);
        if (!s.generated) return false;
        auto local = guides ? std::move(guides)
                            : make_anr_guides(args.device, args.size, guide_interval_ns(args.frameIntervalNs));
        s.guides = make_shared_anr_guides(args.device, args.size,
            guide_interval_ns(args.frameIntervalNs), std::move(local));
        s.bgra = std::make_unique<AnrBgra>(args.device, args.size);
        s.commands = std::make_unique<AnrCommands>(args.device, args.queue);
        s.dispatch = std::make_unique<AnrDispatch>(args.device,
            AnrDispatch::Configuration{args.size, s.generated, args.submissionTrace});
        auto* list = s.commands->begin();
        if (!s.feature->create(list, args.size.width, args.size.height)) {
            s.commands->discard(list);
            return false;
        }
        s.commands->wait_for(s.commands->submit(list));
        faulted_ = false;
        return true;
    } catch (const std::exception& error) {
        if (args.log) fprintf(args.log, "event=anr_runtime_fault reason=initialization message=%s\n", error.what());
        return false;
    }
}

uint32_t AnrFgBackend::multiplier_limit() const noexcept
{
    return faulted_ || !impl_ ? 1u : impl_->generated + 1;
}

std::optional<FgPairTicket> AnrFgBackend::submit_pair(const FgPairDesc& pair, unsigned prevIndex, unsigned currIndex)
{
    if (faulted_ || pair.discontinuity || pair.effectiveMultiplier < 2) return {};
    auto& s = *impl_;
    // A presented pair keeps the single bank reserved: retire_pair names the
    // consumer draw completion point, and no later pair may reuse the guides,
    // commands or dispatch allocations before that fence reaches its value.
    if (s.ticket && (!s.retired || !pair_complete(*s.ticket) || !s.consumer_complete())) return {};
    // NR owns the source chain for this pair. When its exact pair is unavailable
    // the presenter shows the original frame instead of duplicating NR's device
    // analysis locally, and a valid pair without imported motion is equally
    // untrustworthy.
    if (flow_routing(pair.flow) == FlowRouting::NrUnavailable) return {};
    s.ticket.reset();
    const unsigned count = std::min(s.generated, pair.effectiveMultiplier - 1);
    try {
        s.drain();
        s.commands->recycle();
        s.guides->configure(pair.flow);
        if (pair.onStageQueued) pair.onStageQueued();
        // Startup and skipped evaluations need a withheld reset-A.  Use the
        // actually retained input, rather than guessing from transport serial deltas.
        bool ok = true;
        if (!s.previous || s.lastSerial != pair.prevSerial) {
            ok = s.evaluate_frame(pair.prevTex, pair.prevSerial, 1, true);
            if (ok) {
                s.drain();
                s.commands->recycle();
            }
        }
        if (ok) ok = s.evaluate_frame(pair.currTex, pair.currSerial, count, false);
        if (s.gpu) s.gpu->mark_external_inputs_read(prevIndex, currIndex);
        if (!ok) {
            faulted_ = true;
            return {};
        }
    } catch (const std::exception& error) {
        if (s.log) fprintf(s.log, "event=anr_runtime_fault reason=dispatch message=%s\n", error.what());
        faulted_ = true;
        // No frame reuse or original presentation if the consumer restoring
        // COMMON was not submitted and fenced. Generation shutdown owns the drain.
        if (!s.dispatch->input_state_confirmed()) throw;
        if (s.gpu) s.gpu->mark_external_inputs_read(prevIndex, currIndex);
        return {};
    }
    s.selection = {};
    s.ticket.emplace(FgPairTicket{++s.pairId, prevIndex, currIndex, count + 1, count, 0});
    s.retired = false;
    s.consumeFence.Reset();
    s.consumeValue = 0;
    return s.ticket;
}

void AnrFgBackend::retire_pair(const FgPairTicket& ticket, ID3D12Fence* consumeFence, uint64_t consumeValue) noexcept
{
    if (!impl_) return;
    auto& s = *impl_;
    if (!s.ticket || *s.ticket != ticket) return;
    s.consumeFence = consumeFence;
    s.consumeValue = consumeValue;
    s.retired = true;
}

bool AnrFgBackend::pair_complete(const FgPairTicket& ticket) const
{
    const auto& s = *impl_;
    return s.ticket && *s.ticket == ticket && s.commands->complete(s.dispatch->slot(ticket.lastSubmittedSub).fence);
}

AnrFgBackend::Output AnrFgBackend::output(const FgPairTicket& ticket, uint32_t sub)
{
    auto& s = *impl_;
    if (faulted_ || !s.ticket || *s.ticket != ticket || !sub || sub > ticket.lastSubmittedSub) return Output::Suppressed;
    const auto& slot = s.dispatch->slot(sub);
    if (!s.commands->complete(slot.fence)) return Output::Pending;
    auto& value = s.selection[sub - 1];
    if (!value) {
        void* data{};
        const D3D12_RANGE range{0, 1}, noWrite{0, 0};
        const auto hr = slot.readback->Map(0, &range, &data);
        if (FAILED(hr)) {
            if (s.log) fprintf(s.log, "event=anr_runtime_fault reason=disable_readback hr=0x%lX\n",
                static_cast<unsigned long>(hr));
            faulted_ = true;
            return Output::Suppressed;
        }
        value = *static_cast<const uint8_t*>(data) == 0 ? Output::Ready : Output::Suppressed;
        slot.readback->Unmap(0, &noWrite);
    }
    return *value;
}

void AnrFgBackend::resolve_subframe(ID3D12GraphicsCommandList* list, const FgPairTicket& ticket,
    ID3D12Resource* target, uint32_t sub)
{
    if (output(ticket, sub) != Output::Ready) return;
    auto* source = impl_->dispatch->slot(sub).image.Get();
    constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
    constexpr auto src = D3D12_RESOURCE_STATE_COPY_SOURCE, dst = D3D12_RESOURCE_STATE_COPY_DEST;
    transition(list, source, common, src);
    transition(list, target, common, dst);
    list->CopyResource(target, source);
    transition(list, source, src, common);
    transition(list, target, dst, common);
}

void AnrFgBackend::synchronize()
{
    if (impl_) impl_->drain();
}

SharedGuideStats AnrFgBackend::guide_stats() const noexcept
{
    return impl_ && impl_->guides ? impl_->guides->stats() : SharedGuideStats{};
}
} // namespace nr::fg::vfg
