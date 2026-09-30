// SPDX-License-Identifier: GPL-3.0-only
#include "frame_pipeline.h"
#include <array>
#include <atomic>
#include <limits>
#include <semaphore>
#include <stdexcept>
#include <thread>

namespace nr::pipeline {
struct FramePipeline::Impl {
    enum class Stage { Reserved, Queued, Running, Returned };
    struct Slot {
        const Admission admission;
        PublicationState publication = PublicationState::Pending; // Graphics only.
        interop::Ownership physical;
        std::promise<void> signal;
        std::optional<Work> work;
        std::optional<Processed> output; // Worker writes; Returned release publishes.
        std::atomic<Stage> stage{Stage::Reserved};
        Slot(const JobContract& job, size_t index, std::promise<void> promise, std::shared_future<void> future)
            : admission{job, index, std::move(future)},
              physical{job.key, interop::WorkPhase::Captured, 0, 0, 0, 0, 0, 0, false, false, false},
              signal(std::move(promise)) {}
    };
    struct Generation {
        const uint64_t id;
        std::array<std::unique_ptr<Slot>, 3> slots;
        std::shared_ptr<void> featureLease;
        bool featureComplete = true;
        explicit Generation(uint64_t value) : id(value) {}
    };
    const std::thread::id graphics = std::this_thread::get_id();
    std::optional<Epochs> active;
    std::shared_ptr<Processor> processor;
    PerInstanceTick ticks;
    std::unique_ptr<Generation> current;
    std::array<std::unique_ptr<Generation>, 2> retired;
    std::optional<IngressSample> ingress;
    std::optional<TickPlan> plan;
    std::optional<Published> published;
    std::shared_ptr<const RawFrame> previous;
    Slot* reserved = nullptr;
    Phase phase = Phase::Priming;
    uint64_t serial = 0;
    Counters counts{};
    bool accepting = true, closed = false, retirementBlocked = false, epochExhausted = false;
    // Fixed SPSC queue: pointers into at most 3 generations x 3 slots. Never free
    // slots until worker Returned AND producer/worker/consumer ownership resolves.
    std::array<Slot*, 9> queue{};
    std::atomic<uint64_t> written{0}, read{0};
    std::counting_semaphore<16> wake{0};
    std::stop_source stop;
    std::promise<void> exitSignal;
    const std::shared_future<void> exitFuture = exitSignal.get_future().share();
    std::atomic<bool> exited{false};
    std::thread worker;
    Impl(const Epochs& epochs, std::shared_ptr<Processor> p)
        : active(epochs), processor(std::move(p)), ticks(epochs.instanceId), current(std::make_unique<Generation>(epochs.resourceGeneration))
    {
        if (!epochs.valid() || !processor) throw std::invalid_argument("scheduler requires exact epochs and processor");
        worker = std::thread([this] { run(); }); // All signal/lifetime state exists before start.
    }
    bool on_graphics() const { return graphics == std::this_thread::get_id(); }
    template<class F> void each(F&& f) const
    {
        if (current) f(*current);
        for (const auto& g : retired) if (g) f(*g);
    }
    Slot* find(const JobKey& key) const
    {
        Slot* found = nullptr;
        each([&](Generation& g) { for (auto& s : g.slots) if (s && s->admission.job.key == key) found = s.get(); });
        return found;
    }
    Generation* generation(uint64_t id) const
    {
        Generation* found = nullptr;
        each([&](Generation& g) { if (g.id == id) found = &g; });
        return found;
    }
    void run()
    {
        for (;;) {
            wake.acquire(); // Worker only.
            const auto index = read.load(std::memory_order_relaxed);
            if (index != written.load(std::memory_order_acquire)) {
                Slot* s = queue[index % queue.size()];
                read.store(index + 1, std::memory_order_release);
                s->stage.store(Stage::Running, std::memory_order_relaxed);
                auto signal = std::move(s->signal);
                try { s->output.emplace(processor->process(*s->work, stop.get_token())); }
                catch (...) { s->output.emplace(Processed{s->work->job, false, {}}); }
                s->stage.store(Stage::Returned, std::memory_order_release);
                signal.set_value(); // No further slot access: graphics may now collect it.
            }
            if (stop.stop_requested() && read.load(std::memory_order_relaxed) == written.load(std::memory_order_acquire)) break;
        }
        exited.store(true, std::memory_order_release);
        exitSignal.set_value();
    }
    void terminal(Slot& s, PublicationEvent event)
    {
        if (const auto next = advance_publication(s.publication, event)) s.publication = *next;
    }
    void cancel_pending()
    {
        each([&](Generation& g) { for (auto& s : g.slots) if (s) terminal(*s, PublicationEvent::Cancel); });
    }
    void abandon_reserved()
    {
        if (!reserved) return;
        terminal(*reserved, PublicationEvent::Cancel);
        reserved->physical.phase = interop::WorkPhase::Discarded;
        reserved->stage.store(Stage::Returned, std::memory_order_release);
        reserved->signal.set_value();
        reserved = nullptr;
    }
    bool reusable(const Slot& s) const
    {
        if (published && published->selection.job && *published->selection.job == s.admission.job.key) return false;
        return s.stage.load(std::memory_order_acquire) == Stage::Returned &&
            s.publication != PublicationState::Pending && s.publication != PublicationState::Completed &&
            interop::reusable(s.physical);
    }
    void collect()
    {
        each([&](Generation& g) { for (auto& s : g.slots) if (s && reusable(*s)) { s.reset(); ++counts.collectedSlots; } });
        for (auto& g : retired) if (g && g->featureComplete) {
            bool empty = true;
            for (auto& s : g->slots) empty = empty && !s;
            if (empty) { g.reset(); ++counts.collectedGenerations; }
        }
    }
    bool reset(ResetCause cause)
    {
        const auto next = reset_epochs(*active, cause);
        if (!next) { epochExhausted = true; cancel_pending(); return false; }
        collect();
        if (next->next.resourceGeneration != active->resourceGeneration) {
            size_t index = 0;
            for (; index < retired.size() && retired[index]; ++index) {}
            if (index == retired.size()) { retirementBlocked = true; cancel_pending(); return false; }
            auto replacement = std::make_unique<Generation>(next->next.resourceGeneration);
            retired[index] = std::move(current);
            current = std::move(replacement);
        }
        active.emplace(next->next);
        cancel_pending();
        if (next->discardDelayed) { previous.reset(); phase = Phase::Priming; }
        retirementBlocked = false;
        return true;
    }
};
FramePipeline::FramePipeline(const Epochs& e, std::shared_ptr<Processor> p) : impl_(std::make_unique<Impl>(e, std::move(p))) {}
FramePipeline::~FramePipeline()
{
    if (impl_ && !try_close()) (void)impl_.release(); // Retain unresolved thread/code/resources; never force-free.
}
TickPlan FramePipeline::begin(const IngressSample& input, uint64_t deadline)
{
    auto& p = *impl_;
    const Selection rejected{input.canonicalTick, OutputKind::CurrentSource, Status::Bypassed,
        Reason::Cancelled, {}, {}, true};
    if (!p.on_graphics() || !p.accepting) return {TickAction::Reject, rejected, Reason::Cancelled, {}};
    const auto action = p.ticks.claim(input.canonicalTick);
    if (action == TickAction::Reject) return {action, rejected, Reason::OldEpoch, {}};
    if (action != TickAction::CaptureOnce)
        return {action, p.plan->selection, p.plan->admissionReason, p.plan->admission};
    p.abandon_reserved();
    p.published.reset();
    p.collect();
    p.ingress.emplace(input);
    Impl::Slot* old = nullptr;
    if (p.previous) p.each([&](Impl::Generation& g) {
        for (auto& s : g.slots) if (s && s->admission.job.input == p.previous->input) old = s.get();
    });
    std::optional<CompletedFrame> ready;
    if (old && old->stage.load(std::memory_order_acquire) == Impl::Stage::Returned && old->output) {
        const auto& receipt = *old->output;
        if (interop::publishable(old->physical, old->admission.job.key)) p.terminal(*old, PublicationEvent::Complete);
        ready.emplace(CompletedFrame{receipt.job, old->publication,
            interop::completion(old->physical.producer, old->physical.producerDone) == interop::Completion::Complete,
            interop::completion(old->physical.worker, old->physical.workerDone) == interop::Completion::Complete,
            receipt.evaluated && static_cast<bool>(receipt.image)});
    }
    const auto selected = select_for_tick(*p.active, p.phase, input,
        p.previous ? &p.previous->input : nullptr, old ? &old->admission.job : nullptr, ready ? &*ready : nullptr);
    std::shared_ptr<const void> image;
    std::shared_ptr<const RawFrame> raw;
    if (selected.kind == OutputKind::DelayedNr) {
        image = old->output->image;
        raw = p.previous;
        p.terminal(*old, PublicationEvent::Publish);
    } else if (selected.kind == OutputKind::DelayedRaw) {
        image = p.previous->image;
        raw = p.previous;
        if (old) p.terminal(*old, PublicationEvent::Expire);
    }
    if (selected.resetTemporal) {
        const auto cause = selected.kind == OutputKind::DelayedRaw ?
            (selected.reason == Reason::EvaluationFailure ? ResetCause::EvaluationFailure : ResetCause::DeadlineMiss) : ResetCause::InvalidInput;
        (void)p.reset(cause);
    } else if (!input.settings.enabled) { p.previous.reset(); p.phase = Phase::Priming; p.cancel_pending(); }
    p.published.emplace(Published{selected, raw, image}); // Protect selected transport before admission/collection.
    p.collect();
    std::optional<Admission> admission;
    Reason reason = input_problem(input, *p.active);
    if (!input.settings.enabled) reason = Reason::Cancelled;
    else if (p.retirementBlocked) reason = Reason::RetirementBudget;
    else if (p.epochExhausted || deadline <= input.canonicalTick || p.serial == std::numeric_limits<uint64_t>::max()) reason = Reason::KeyMismatch;
    if (reason == Reason::None) {
        size_t index = 0;
        for (; index < p.current->slots.size() && p.current->slots[index]; ++index) {}
        if (index == p.current->slots.size()) { reason = Reason::SlotsFull; (void)p.reset(ResetCause::EvaluationSkipped); }
        else {
            const JobContract job{{input.key, p.active->historyEpoch, p.active->resourceGeneration, ++p.serial}, input, deadline};
            std::promise<void> signal;
            auto future = signal.get_future().share();
            auto slot = std::make_unique<Impl::Slot>(job, index, std::move(signal), future);
            admission.emplace(slot->admission);
            p.reserved = slot.get();
            p.current->slots[index] = std::move(slot);
        }
    }
    p.plan.emplace(TickPlan{action, selected, reason, admission});
    p.published.emplace(Published{selected, raw, image});
    ++p.counts.selections;
    return *p.plan;
}
bool FramePipeline::finish(std::shared_ptr<const RawFrame> raw, const WorkBinding* binding)
{
    auto& p = *impl_;
    if (!p.on_graphics() || !p.accepting || !p.plan || !p.ingress || p.ticks.peek(p.ingress->canonicalTick)) return false;
    const bool valid = input_problem(*p.ingress, *p.active) == Reason::None && p.ingress->settings.enabled;
    if (valid && (!raw || !raw->image || raw->input != *p.ingress)) return false;
    if (binding) {
        if (!valid || !p.reserved || binding->ticket.key != p.reserved->admission.job.key ||
            binding->ticket.slot != p.reserved->admission.slot || !binding->captured.key ||
            *binding->captured.key != binding->ticket.key || binding->captured.phase != interop::WorkPhase::Captured ||
            binding->captured.failed || binding->captured.cancelled || !binding->captured.producer ||
            (!binding->transport && !binding->processorLease)) return false;
        if (binding->captured.producer == UINT64_MAX || binding->captured.producerDone == UINT64_MAX ||
            binding->captured.workerDone == UINT64_MAX || binding->captured.consumerDone == UINT64_MAX) return false;
        if (binding->transport && (!binding->capturePin || !binding->capturePin->raw ||
            binding->capturePin->ticket != binding->ticket || !binding->capturePin->texture ||
            !binding->captured.consumerOpen || !binding->captured.consumer ||
            binding->capturePin->completionValue != binding->captured.consumer)) return false;
        auto& s = *p.reserved;
        s.work.emplace(Work{s.admission.job, raw, *binding});
        std::destroy_at(&s.physical); // Ownership contains the frozen nonassignable JobKey.
        std::construct_at(&s.physical, binding->captured);
        s.stage.store(Impl::Stage::Queued, std::memory_order_relaxed);
        const auto index = p.written.load(std::memory_order_relaxed);
        p.queue[index % p.queue.size()] = &s;
        p.written.store(index + 1, std::memory_order_release);
        p.reserved = nullptr;
        ++p.counts.dispatches;
        p.wake.release();
    } else if (p.reserved) { p.abandon_reserved(); (void)p.reset(ResetCause::EvaluationSkipped); }
    if (p.plan->selection.kind == OutputKind::PrimingRaw)
        p.published.emplace(Published{p.plan->selection, raw, raw ? raw->image : nullptr});
    if (valid) { p.previous = std::move(raw); p.phase = Phase::Delayed; ++p.counts.captures; }
    else { p.previous.reset(); p.phase = Phase::Priming; }
    return p.ticks.latch(p.ingress->canonicalTick, p.published->selection);
}
const Published* FramePipeline::view(uint64_t tick) const
{
    return impl_->on_graphics() && impl_->ticks.peek(tick) && impl_->published ? &*impl_->published : nullptr;
}
bool FramePipeline::observe(const interop::Ticket& ticket, const interop::Ownership& receipt)
{
    auto& p = *impl_;
    if (!p.on_graphics()) return false;
    auto* s = p.find(ticket.key);
    if (!s || ticket.slot != s->admission.slot || !receipt.key || *receipt.key != ticket.key) return false;
    const auto& before = s->physical;
    // Reject malformed/reordered physical receipts. No failed sentinel can masquerade
    // as completion; publication cancels but physical ownership stays retained.
    if (receipt.producerDone == UINT64_MAX || receipt.workerDone == UINT64_MAX || receipt.consumerDone == UINT64_MAX ||
        receipt.producer == UINT64_MAX || receipt.worker == UINT64_MAX || receipt.consumer == UINT64_MAX) {
        p.terminal(*s, PublicationEvent::Cancel);
        return false;
    }
    if (receipt.phase == interop::WorkPhase::Empty || receipt.producer < before.producer || receipt.worker < before.worker ||
        receipt.consumer < before.consumer || receipt.producerDone < before.producerDone ||
        receipt.workerDone < before.workerDone || receipt.consumerDone < before.consumerDone ||
        (before.phase == interop::WorkPhase::Submitted && receipt.phase != interop::WorkPhase::Submitted) ||
        (before.phase == interop::WorkPhase::Discarded && receipt.phase != interop::WorkPhase::Discarded) ||
        (receipt.phase == interop::WorkPhase::Submitted && !receipt.worker)) return false;
    const bool failed = before.failed || receipt.failed;
    const bool cancelled = before.cancelled || receipt.cancelled;
    std::destroy_at(&s->physical);
    std::construct_at(&s->physical, receipt);
    s->physical.failed = failed;
    s->physical.cancelled = cancelled;
    if (s->physical.failed || s->physical.cancelled) p.terminal(*s, PublicationEvent::Cancel);
    return true;
}
bool FramePipeline::cancel(const JobKey& key)
{
    if (!impl_->on_graphics()) return false;
    auto* s = impl_->find(key);
    if (!s) return false;
    impl_->terminal(*s, PublicationEvent::Cancel);
    return true;
}
std::optional<SlotState> FramePipeline::state(const JobKey& key) const
{
    if (!impl_->on_graphics()) return {};
    const auto* s = impl_->find(key);
    if (!s) return {};
    return SlotState{{s->admission.job.key, s->admission.slot}, s->publication, s->physical,
        s->stage.load(std::memory_order_acquire) == Impl::Stage::Returned};
}
bool FramePipeline::hold_generation(uint64_t id, std::shared_ptr<void> lease)
{
    if (!impl_->on_graphics()) return false;
    auto* g = impl_->generation(id);
    if (!impl_->accepting || !g || !lease || g->featureLease) return false;
    g->featureLease = std::move(lease);
    g->featureComplete = false;
    return true;
}
bool FramePipeline::complete_generation(uint64_t id)
{
    if (!impl_->on_graphics()) return false;
    auto* g = impl_->generation(id);
    if (!g || !g->featureLease || g->featureComplete) return false;
    g->featureComplete = true;
    return true;
}
bool FramePipeline::reset(ResetCause cause)
{
    if (!impl_->on_graphics() || !impl_->accepting) return false;
    impl_->abandon_reserved();
    return impl_->reset(cause);
}
Epochs FramePipeline::epochs() const { return *impl_->active; }
size_t FramePipeline::retired_generations() const
{
    size_t count = 0;
    for (const auto& g : impl_->retired) if (g) ++count;
    return count;
}
Counters FramePipeline::counters() const { return impl_->counts; }
void FramePipeline::collect() { if (impl_->on_graphics()) impl_->collect(); }
std::shared_future<void> FramePipeline::worker_exit() const { return impl_->exitFuture; }
void FramePipeline::request_stop()
{
    auto& p = *impl_;
    if (!p.on_graphics() || !p.accepting) return;
    p.accepting = false;
    p.abandon_reserved();
    p.cancel_pending();
    p.published.reset();
    p.previous.reset();
    p.stop.request_stop();
    p.wake.release();
}
bool FramePipeline::try_close()
{
    auto& p = *impl_;
    if (!p.on_graphics()) return false;
    if (p.closed) return true;
    request_stop();
    p.collect();
    if (!p.exited.load(std::memory_order_acquire) || retired_generations() || !p.current->featureComplete) return false;
    for (const auto& s : p.current->slots) if (s) return false;
    // Lifecycle close only, after worker's exit receipt; no render-path join.
    p.worker.join();
    p.current.reset();
    p.processor.reset();
    p.closed = true;
    return true;
}
} // namespace nr::pipeline
