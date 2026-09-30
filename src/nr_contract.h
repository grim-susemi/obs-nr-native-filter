// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Original native contracts; R1/R2 are behavior/provenance references, not copied source.
// No OBS/D3D dependency, clock, global routing or worker implementation here.
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace nr {
struct SettingsRevision {
    const uint64_t value;
    bool operator==(const SettingsRevision&) const = default;
};
struct CompositionStrength {
    const float value;
    bool valid() const { return std::isfinite(value) && value >= 0 && value <= 1; }
    bool operator==(const CompositionStrength&) const = default;
};
// Tuning is captured from the owned core defaults, not re-normalized on submission.
// Preserve the skinStructure=-1 sentinel; vendor tuning admission belongs to the core.
struct ModelTuning {
    const uint32_t preset, style;
    const float intensity, localStructure, localTone, skinStructure;
    const bool autoMask;
    bool valid() const
    {
        return std::isfinite(intensity) && std::isfinite(localStructure) &&
               std::isfinite(localTone) && std::isfinite(skinStructure);
    }
    bool operator==(const ModelTuning&) const = default;
};
struct Settings {
    const SettingsRevision revision;
    const bool enabled;
    const std::string runtimePath; // Loader validates the canonical path/PE/exports separately.
    const CompositionStrength strength;
    const ModelTuning tuning;
    bool valid() const { return revision.value != 0 && !runtimePath.empty() && strength.valid() && tuning.valid(); }
    bool operator==(const Settings&) const = default;
};

enum class PixelFormat { Unknown, Bgra8, Rgba8, Rgba16Float, Rgb10A2 };
enum class ColorSpace { Unknown, Rec709Srgb, ScRgbLinear, Rec2020Linear, Rec2020Pq };
enum class AlphaMode { Unknown, Opaque, Straight, Premultiplied };
struct Extent {
    const uint32_t width, height;
    bool operator==(const Extent&) const = default;
};
struct Rect {
    const uint32_t x, y, width, height;
    bool operator==(const Rect&) const = default;
};
struct FrameDescriptor {
    const Extent allocation;
    const Rect active;
    const PixelFormat format;
    const ColorSpace color;
    const AlphaMode alpha;
    const float referenceWhiteNits;
    bool valid() const
    {
        if (!allocation.width || !allocation.height || !active.width || !active.height ||
            active.x > allocation.width || active.y > allocation.height ||
            active.width > allocation.width - active.x || active.height > allocation.height - active.y ||
            !std::isfinite(referenceWhiteNits) || referenceWhiteNits <= 0)
            return false;
        if (alpha != AlphaMode::Opaque && alpha != AlphaMode::Straight && alpha != AlphaMode::Premultiplied)
            return false;
        switch (format) {
        case PixelFormat::Bgra8: case PixelFormat::Rgba8: return color == ColorSpace::Rec709Srgb;
        case PixelFormat::Rgba16Float:
            return color == ColorSpace::ScRgbLinear || color == ColorSpace::Rec2020Linear;
        case PixelFormat::Rgb10A2: return color == ColorSpace::Rec2020Pq;
        default: return false;
        }
    }
    bool operator==(const FrameDescriptor&) const = default;
};

// All IDs are nonzero monotonic tokens. Instance IDs outlive address reuse; ingress
// sequences advance on capture, including identical pixels. No HDMI/source PTS is invented.
struct FrameKey {
    const uint64_t instanceId, contentEpoch, ingressSequence;
    bool valid() const { return instanceId && contentEpoch && ingressSequence; }
    bool operator==(const FrameKey&) const = default;
};
struct JobKey {
    const FrameKey frame;
    const uint64_t historyEpoch, resourceGeneration, submissionSerial;
    bool valid() const { return frame.valid() && historyEpoch && resourceGeneration && submissionSerial; }
    bool operator==(const JobKey&) const = default;
};
struct Epochs {
    const uint64_t instanceId, contentEpoch, historyEpoch, resourceGeneration;
    bool valid() const { return instanceId && contentEpoch && historyEpoch && resourceGeneration; }
    bool operator==(const Epochs&) const = default;
};
enum class SourceValidity { Unknown, Valid, Invalid };
struct IngressSample {
    const FrameKey key;
    const uint64_t canonicalTick; // OBS canonical video tick token, NOT upstream PTS.
    const uint64_t precedingCanonicalTick; // Explicit OBS predecessor, not the last successful capture.
    const uint64_t sourceIdentity, deviceIdentity, resourceGeneration;
    const SourceValidity validity; // Explicit upstream validity; pixel repetition is not loss.
    const FrameDescriptor descriptor;
    const Settings settings;
    bool operator==(const IngressSample&) const = default;
};
struct JobContract {
    const JobKey key;
    const IngressSample input; // Private immutable raw and model inputs bind this same sample.
    const uint64_t deadlineTick;
    bool operator==(const JobContract&) const = default;
};
enum class PublicationState { Pending, Completed, Published, Expired, Cancelled };
enum class PublicationEvent { Complete, Publish, Expire, Cancel };
// Expiry/cancellation is irrevocable even when the actual GPU work later completes.
inline std::optional<PublicationState> advance_publication(PublicationState state, PublicationEvent event)
{
    if (state == PublicationState::Expired || state == PublicationState::Cancelled || state == PublicationState::Published)
        return std::nullopt;
    switch (event) {
    case PublicationEvent::Complete:
        if (state == PublicationState::Pending) return PublicationState::Completed;
        break;
    case PublicationEvent::Publish:
        if (state == PublicationState::Completed) return PublicationState::Published;
        break;
    case PublicationEvent::Expire: return PublicationState::Expired;
    case PublicationEvent::Cancel: return PublicationState::Cancelled;
    }
    return std::nullopt;
}
struct CompletedFrame {
    const JobContract job;
    const PublicationState publication;
    const bool producerComplete, workerComplete, evaluated;
};
enum class Phase { Priming, Delayed };
enum class OutputKind { CurrentSource, PrimingRaw, DelayedRaw, DelayedNr };
enum class Status { Disabled, Priming, Applied, Bypassed, Error };
enum class Reason { None, MissingIdentity, InvalidSource, UnknownValidity, InvalidDescriptor,
    InvalidSettings, OldEpoch, KeyMismatch, MissingPrevious, DeadlineMiss, Expired, Cancelled,
    EvaluationFailure, SlotsFull, RetirementBudget };
struct Selection {
    const uint64_t canonicalTick;
    const OutputKind kind;
    const Status status;
    const Reason reason;
    const std::optional<FrameKey> frame;
    const std::optional<JobKey> job;
    const bool resetTemporal; // Both NR and OFA, never one independently.
};

// Policy helpers follow. Actual fences/resources and async execution belong to Task7/10/13.
inline Reason input_problem(const IngressSample& input, const Epochs& active)
{
    if (!input.key.valid() || !input.canonicalTick || !input.sourceIdentity || !input.deviceIdentity || !active.valid())
        return Reason::MissingIdentity;
    if (input.precedingCanonicalTick >= input.canonicalTick) return Reason::KeyMismatch;
    if (input.key.instanceId != active.instanceId || input.key.contentEpoch != active.contentEpoch)
        return Reason::OldEpoch;
    if (input.resourceGeneration != active.resourceGeneration) return Reason::KeyMismatch;
    if (input.validity == SourceValidity::Invalid) return Reason::InvalidSource;
    if (input.validity != SourceValidity::Valid) return Reason::UnknownValidity;
    if (!input.descriptor.valid()) return Reason::InvalidDescriptor;
    if (!input.settings.valid()) return Reason::InvalidSettings;
    return Reason::None;
}
inline Reason completion_problem(const JobContract& expected, const CompletedFrame& ready,
                                 const Epochs& active, uint64_t tick)
{
    if (!expected.key.valid() || expected.key.frame != expected.input.key ||
        expected.key.historyEpoch != active.historyEpoch || expected.key.resourceGeneration != active.resourceGeneration ||
        expected.input.resourceGeneration != active.resourceGeneration || ready.job != expected || expected.deadlineTick != tick)
        return Reason::KeyMismatch;
    if (const auto problem = input_problem(expected.input, active); problem != Reason::None) return problem;
    if (ready.publication == PublicationState::Expired) return Reason::Expired;
    if (ready.publication == PublicationState::Cancelled) return Reason::Cancelled;
    if (ready.publication != PublicationState::Completed || !ready.producerComplete || !ready.workerComplete)
        return Reason::DeadlineMiss;
    if (!ready.evaluated) return Reason::EvaluationFailure;
    return Reason::None;
}
// Caller snapshots completion BEFORE selection and then latches this value for every view.
// Raw means the untouched private snapshot, not a recapture or current-source substitute.
inline Selection select_for_tick(const Epochs& active, Phase phase, const IngressSample& current,
                                 const IngressSample* previous = nullptr, const JobContract* expected = nullptr,
                                 const CompletedFrame* ready = nullptr)
{
    if (!current.settings.enabled)
        return {current.canonicalTick, OutputKind::CurrentSource, Status::Disabled, Reason::None, std::nullopt, std::nullopt, false};
    if (const auto problem = input_problem(current, active); problem != Reason::None)
        return {current.canonicalTick, OutputKind::CurrentSource, Status::Bypassed, problem, std::nullopt, std::nullopt, true};
    if (phase == Phase::Priming)
        return {current.canonicalTick, OutputKind::PrimingRaw, Status::Priming, Reason::None, current.key, std::nullopt, false};
    if (!previous)
        return {current.canonicalTick, OutputKind::CurrentSource, Status::Bypassed, Reason::MissingPrevious, std::nullopt, std::nullopt, true};
    if (const auto problem = input_problem(*previous, active); problem != Reason::None)
        return {current.canonicalTick, OutputKind::CurrentSource, Status::Bypassed, problem, std::nullopt, std::nullopt, true};
    if (previous->key.ingressSequence >= current.key.ingressSequence ||
        current.key.ingressSequence - previous->key.ingressSequence != 1 ||
        previous->canonicalTick != current.precedingCanonicalTick || previous->sourceIdentity != current.sourceIdentity ||
        previous->deviceIdentity != current.deviceIdentity || previous->settings != current.settings ||
        previous->descriptor != current.descriptor)
        return {current.canonicalTick, OutputKind::CurrentSource, Status::Bypassed, Reason::KeyMismatch, std::nullopt, std::nullopt, true};
    Reason problem = Reason::DeadlineMiss;
    if (expected && expected->input != *previous) problem = Reason::KeyMismatch;
    else if (expected && ready) problem = completion_problem(*expected, *ready, active, current.canonicalTick);
    if (problem == Reason::None)
        return {current.canonicalTick, OutputKind::DelayedNr, Status::Applied, Reason::None, previous->key, expected->key, false};
    return {current.canonicalTick, OutputKind::DelayedRaw, problem == Reason::EvaluationFailure ? Status::Error : Status::Bypassed,
            problem, previous->key, std::nullopt, true};
}

enum class TickAction { CaptureOnce, AwaitLatch, ReuseLatch, Reject };
// Only canonical capture/latch ownership, not a scheduler. One object per instance.
// Calls are serialized by the OBS graphics-thread owner; no locking or foreign calls.
class PerInstanceTick {
    const uint64_t instanceId_;
    uint64_t tick_ = 0;
    std::optional<Selection> selection_;
public:
    explicit PerInstanceTick(uint64_t instanceId) : instanceId_(instanceId) {}
    PerInstanceTick(const PerInstanceTick&) = delete;
    PerInstanceTick& operator=(const PerInstanceTick&) = delete;
    TickAction claim(uint64_t tick)
    {
        if (!instanceId_ || !tick || tick < tick_) return TickAction::Reject;
        if (tick == tick_) return selection_ ? TickAction::ReuseLatch : TickAction::AwaitLatch;
        tick_ = tick;
        selection_.reset();
        return TickAction::CaptureOnce;
    }
    bool latch(uint64_t tick, const Selection& selection)
    {
        if (!tick_ || tick != tick_ || selection.canonicalTick != tick || selection_ ||
            (selection.frame && selection.frame->instanceId != instanceId_)) return false;
        selection_.emplace(selection);
        return true;
    }
    const Selection* peek(uint64_t tick) const
    {
        return tick == tick_ && selection_ ? &*selection_ : nullptr;
    }
};
enum class ResetCause { SourceChanged, Cut, InvalidInput, SettingsChanged, ExtentChanged,
    FormatChanged, DeviceChanged, Disabled, EvaluationSkipped, EvaluationFailure, DeadlineMiss };
struct ResetTransition {
    const Epochs next;
    const bool resetNr, resetOfa, discardDelayed;
};
// Missed/skipped/failed evaluation preserves raw n-1 phase, but resets BOTH histories.
// Source/settings/device/cut/etc discard delayed eligibility, then explicitly prime again.
// Resource replacement is admitted only after checking the retirement budget below.
inline std::optional<ResetTransition> reset_epochs(const Epochs& before, ResetCause cause)
{
    const bool content = cause != ResetCause::EvaluationSkipped && cause != ResetCause::EvaluationFailure &&
                         cause != ResetCause::DeadlineMiss;
    const bool resource = cause == ResetCause::SettingsChanged || cause == ResetCause::ExtentChanged ||
                          cause == ResetCause::FormatChanged || cause == ResetCause::DeviceChanged;
    const auto max = std::numeric_limits<uint64_t>::max();
    if (!before.valid() || before.historyEpoch == max || (content && before.contentEpoch == max) ||
        (resource && before.resourceGeneration == max)) return std::nullopt;
    return ResetTransition{{before.instanceId, before.contentEpoch + (content ? 1 : 0), before.historyEpoch + 1,
                            before.resourceGeneration + (resource ? 1 : 0)}, true, true, content};
}

struct SlotOwnership {
    const std::optional<JobKey> owner;
    const bool submitted, recordingDiscarded, producerComplete, workerComplete, consumerComplete;
};
// Publication state is deliberately absent: expiry/cancel is NOT real completion.
inline bool can_reuse(const SlotOwnership& slot)
{
    if (!slot.owner) return true;
    return slot.producerComplete && slot.consumerComplete &&
           (slot.submitted ? slot.workerComplete : slot.recordingDiscarded);
}
inline std::optional<size_t> admit_slot(std::span<const SlotOwnership> slots)
{
    if (slots.size() != 3) return std::nullopt;
    for (size_t i = 0; i < slots.size(); ++i) if (can_reuse(slots[i])) return i;
    return std::nullopt; // Visible SlotsFull bypass/admission skip; never wait or allocate slot4.
}
struct RetiredGeneration {
    const uint64_t generation;
    const std::array<SlotOwnership, 3> slots;
    const bool featureComplete, recordingsDiscarded;
};
inline bool can_collect(const RetiredGeneration& retired)
{
    if (!retired.featureComplete || !retired.recordingsDiscarded) return false;
    for (const auto& slot : retired.slots) if (!can_reuse(slot)) return false;
    return true;
}
enum class RetirementAction { Retire, AlreadyRetired, BudgetExhausted, Reject };
inline RetirementAction admit_retirement(uint64_t generation, std::span<const RetiredGeneration> retired)
{
    if (!generation || retired.size() > 2) return RetirementAction::Reject;
    for (const auto& entry : retired) if (entry.generation == generation) return RetirementAction::AlreadyRetired;
    return retired.size() == 2 ? RetirementAction::BudgetExhausted : RetirementAction::Retire;
}
} // namespace nr
