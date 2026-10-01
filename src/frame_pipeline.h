// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "interop.h"
#include <future>
#include <memory>
#include <stop_token>

namespace nr::pipeline {
// OWNED immutable snapshot, independent of Bridge's three slots. Task11 supplies
// graphics-owned texture lifetime; the worker never borrows an OBS source texture.
struct RawFrame {
    const IngressSample input;
    const std::shared_ptr<const void> image;
};
struct WorkBinding {
    const interop::Ticket ticket;
    const interop::Ownership captured;
    const std::shared_ptr<interop::Bridge> transport;
    const std::shared_ptr<void> processorLease; // Model/guide/queue lifetime, or explicit CPU fixture.
    // Real Bridge capture MUST be pinned against early physical-slot reuse until
    // its deadline selection. Graphics ends this raw Consumer before acquiring the
    // selected output Consumer, then delivers the actual updated ownership receipt.
    const std::optional<interop::Consumer> capturePin = std::nullopt;
};
struct Work {
    const JobContract job;
    const std::shared_ptr<const RawFrame> raw;
    const WorkBinding binding;
};
struct Processed {
    const JobContract job;
    const bool evaluated;
    const std::shared_ptr<const void> image;
};
// Exactly one worker calls this seam. Later Task11 records/submits real NR here.
// May wait only producer dependencies. No OBS immediate context. Return is NOT
// physical GPU completion; graphics delivers exact observe receipts separately.
class Processor {
public:
    virtual ~Processor() = default;
    virtual Processed process(const Work&, std::stop_token) = 0;
    virtual void worker_stopping() noexcept {}
};
struct Admission {
    const JobContract job;
    const size_t slot;
    const std::shared_future<void> returned; // Subscribe BEFORE finish/dispatch.
};
struct TickPlan {
    const TickAction action;
    const Selection selection;
    const Reason admissionReason;
    const std::optional<Admission> admission;
};
struct Published {
    const Selection selection;
    const std::shared_ptr<const RawFrame> raw;
    const std::shared_ptr<const void> image; // Exact same-phase raw or NR; null for placeholder.
};
struct SlotState {
    const interop::Ticket ticket;
    const PublicationState publication;
    const interop::Ownership physical;
    const bool dispatchReturned;
};
struct Counters {
    uint64_t captures, dispatches, selections, collectedSlots, collectedGenerations;
};
class FramePipeline final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    FramePipeline(const Epochs&, std::shared_ptr<Processor>);
    ~FramePipeline();
    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;
    // Graphics-owner-only. begin is the readiness latch BEFORE foreign capture/draw.
    // Deadline is the caller's canonical successor, not a synthesized source PTS.
    TickPlan begin(const IngressSample&, uint64_t nextCanonicalTick);
    bool finish(std::shared_ptr<const RawFrame>, const WorkBinding* = nullptr);
    const Published* view(uint64_t canonicalTick) const;
    bool observe(const interop::Ticket&, const interop::Ownership&);
    bool cancel(const JobKey&);
    std::optional<SlotState> state(const JobKey&) const;
    // Task13 future lifecycle boundary: hold feature/upload generation independently
    // of slots, resolve only by exact-generation receipt. Open consumer stays live.
    bool hold_generation(uint64_t generation, std::shared_ptr<void>);
    bool complete_generation(uint64_t generation);
    bool reset(ResetCause);
    Epochs epochs() const;
    size_t retired_generations() const;
    Counters counters() const;
    void collect();
    std::shared_future<void> worker_exit() const; // Subscribe BEFORE request_stop.
    void request_stop();
    bool try_close(); // Nonblocking refusal while ANY ownership is unresolved.
    // Destruction on refusal retains worker/resource state. Caller MUST keep plugin
    // code loaded and retry close. Arbitrary external hangs aren't contained.
};
} // namespace nr::pipeline
