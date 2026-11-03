// SPDX-License-Identifier: GPL-3.0-only
// Original tests. Recorded OptiScaler tests are precedents, not copied code.
#include "nr_contract.h"
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

using namespace nr;
namespace {
unsigned checks = 0;
void expect(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
Settings settings(uint64_t revision = 1, float strength = .75f, bool enabled = true)
{
    return {{revision}, enabled, "C:/owned/runtime.dll", {strength}, {0, 0, 1, 1, 0, -1, true}};
}
FrameDescriptor descriptor(uint32_t width = 1920, uint32_t height = 1080)
{
    return {{width, height}, {0, 0, width, height}, PixelFormat::Bgra8,
            ColorSpace::Rec709Srgb, AlphaMode::Premultiplied, 80.0f};
}
IngressSample sample(uint64_t sequence, uint64_t tick, uint64_t instance = 41,
                     uint64_t content = 1, uint64_t generation = 1, uint64_t revision = 1,
                     uint64_t source = 301, uint32_t width = 1920,
                     SourceValidity validity = SourceValidity::Valid)
{
    return {{instance, content, sequence}, tick, tick == 0 ? 0 : tick - 1, source, 901, generation, validity,
            descriptor(width), settings(revision)};
}
JobContract job(const IngressSample& input, uint64_t history, uint64_t serial, uint64_t deadline)
{
    return {{input.key, history, input.resourceGeneration, serial}, input, deadline};
}
CompletedFrame complete(const JobContract& submitted, PublicationState state = PublicationState::Completed,
                        bool worker = true, bool producer = true, bool evaluated = true)
{
    return {submitted, state, producer, worker, evaluated};
}

// Independent content oracle: prime raw 0, then emit 0,1,2,3,4,5.
// Kinds: raw,NR,raw,NR,NR,raw,NR. Between-view completion cannot alter raw 1.
void known_sequence(bool print)
{
    PerInstanceTick views(41);
    struct Fixture { const FrameKey key; const uint64_t content; const std::array<uint8_t, 4> raw; };
    const std::array<Fixture, 7> ingress{{
        {{41, 1, 1}, 0, {11, 22, 33, 44}}, {{41, 1, 2}, 1, {55, 66, 77, 88}},
        {{41, 1, 3}, 2, {99, 111, 122, 133}}, {{41, 1, 4}, 3, {144, 155, 166, 177}},
        {{41, 1, 5}, 4, {188, 199, 211, 222}}, {{41, 1, 6}, 5, {233, 244, 255, 17}},
        {{41, 1, 7}, 6, {28, 39, 41, 52}}}};
    const auto originalIngress = ingress;
    const auto content = [&](const FrameKey& key) -> const Fixture& {
        for (const auto& input : ingress) if (input.key == key) return input;
        throw std::runtime_error("selected key absent from independent ingress fixture");
    };
    const std::array<uint64_t, 7> expectedIds{0, 0, 1, 2, 3, 4, 5};
    const std::array<OutputKind, 7> expectedKinds{OutputKind::PrimingRaw, OutputKind::DelayedNr,
        OutputKind::DelayedRaw, OutputKind::DelayedNr, OutputKind::DelayedNr,
        OutputKind::DelayedRaw, OutputKind::DelayedNr};
    const std::array<uint64_t, 7> histories{1, 1, 1, 1, 1, 1, 1}; // late jobs keep the history epoch
    const std::array<std::array<const char*, 3>, 7> orders{{
        {"preview", "program", "projector"}, {"program", "projector", "preview"},
        {"projector", "preview", "program"}, {"preview", "projector", "program"},
        {"program", "preview", "projector"}, {"projector", "program", "preview"},
        {"preview", "program", "projector"}}};
    unsigned captures = 0;
    for (uint64_t n = 0; n < expectedIds.size(); ++n) {
        const auto current = sample(n + 1, 100 + n);
        const auto previous = sample(n == 0 ? 1 : n, n == 0 ? 100 : 99 + n);
        const Epochs active{41, 1, histories[n], 1};
        const auto submitted = job(previous, histories[n], n + 1, 100 + n);
        const bool miss = n == 2 || n == 5;
        const auto ready = complete(submitted, miss ? PublicationState::Pending : PublicationState::Completed, !miss);
        expect(views.claim(current.canonicalTick) == TickAction::CaptureOnce, "new tick not captured");
        ++captures;
        const auto selected = select_for_tick(active, n == 0 ? Phase::Priming : Phase::Delayed,
                                             current, &previous, &submitted, &ready);
        expect(selected.kind == expectedKinds[n], "independent NR/raw oracle mismatch");
        expect(selected.frame && content(*selected.frame).content == expectedIds[n], "one-tick oracle mismatch");
        const auto index = static_cast<size_t>(expectedIds[n]);
        expect(content(*selected.frame).raw == originalIngress[index].raw, "fallback did not bind untouched fixture raw");
        expect(!selected.resetTemporal, "a late job must keep the joint NR/OFA temporal history");
        expect(views.latch(current.canonicalTick, selected), "first selection not latched");
        const auto late = complete(submitted);
        const auto afterDeadline = select_for_tick(active, Phase::Delayed, current, &previous, &submitted, &late);
        expect(!views.latch(current.canonicalTick, afterDeadline), "between-view completion changed selection");
        for (const char* view : orders[n]) {
            expect(views.claim(current.canonicalTick) == TickAction::ReuseLatch, "duplicate view recaptured");
            const auto* latched = views.peek(current.canonicalTick);
            expect(latched && latched->kind == expectedKinds[n] &&
                   content(*latched->frame).content == expectedIds[n], "view order changed content");
            if (print) std::printf("tick=%llu view=%s content=%llu kind=%u\n",
                static_cast<unsigned long long>(current.canonicalTick), view,
                static_cast<unsigned long long>(content(*latched->frame).content), static_cast<unsigned>(latched->kind));
        }
    }
    expect(captures == 7, "one capture per tick mismatch");
    expect(views.claim(99) == TickAction::Reject, "old tick admitted");
    expect(!views.peek(99), "old tick exposed cached output");
}

void immutable_settings()
{
    static_assert(!std::is_copy_assignable_v<Settings>);
    static_assert(!std::is_copy_assignable_v<ModelTuning>);
    static_assert(!std::is_copy_assignable_v<IngressSample>);
    static_assert(!std::is_copy_assignable_v<FrameDescriptor>);
    static_assert(!std::is_copy_assignable_v<FrameKey> && !std::is_copy_assignable_v<JobKey>);
    static_assert(std::is_same_v<decltype(Settings::revision), const SettingsRevision>);
    static_assert(std::is_same_v<decltype(Settings::strength), const CompositionStrength>);
    const auto old = settings();
    const auto next = settings(2, .25f);
    expect(old.revision.value == 1 && old.strength.value == .75f && old.tuning.skinStructure == -1,
           "new settings modified captured settings");
    expect(next.revision.value == 2 && next.strength.value == .25f && old != next, "revision not distinguished");
    const Settings captured = old;
    expect(captured.tuning == old.tuning && captured.runtimePath == old.runtimePath, "settings copy lost typed data");
    expect(!settings(0).valid() && !settings(1, -1).valid() && !settings(1, 1.01f).valid(), "invalid settings admitted");
    expect(!settings(1, std::numeric_limits<float>::quiet_NaN()).valid(), "nonfinite strength admitted");
}
void adversarial(bool print)
{
    const Epochs epochs{41, 1, 1, 1};
    const auto current = sample(2, 101);
    const auto previous = sample(1, 100);
    const auto expected = job(previous, 1, 7, 101);
    const auto good = complete(expected);
    expect(select_for_tick(epochs, Phase::Delayed, current, &previous, &expected, &good).kind == OutputKind::DelayedNr,
           "known exact completion rejected");
    const std::array<JobKey, 8> wrongKeys{{
        {{42, 1, 1}, 1, 1, 7}, {{41, 2, 1}, 1, 1, 7}, {{41, 1, 2}, 1, 1, 7},
        {{41, 1, 1}, 2, 1, 7}, {{41, 1, 1}, 1, 2, 7}, {{41, 1, 1}, 1, 1, 8},
        {{0, 1, 1}, 1, 1, 7}, {{41, 1, 1}, 0, 1, 7}}};
    for (const auto& key : wrongKeys) {
        const JobContract wrong{key, previous, 101};
        const auto ready = complete(wrong);
        const auto selected = select_for_tick(epochs, Phase::Delayed, current, &previous, &expected, &ready);
        expect(selected.kind == OutputKind::DelayedRaw && selected.frame == previous.key &&
               selected.reason == Reason::KeyMismatch, "wrong job key admitted or wrong raw chosen");
    }
    const std::array<IngressSample, 6> mutatedInputs{{sample(1, 100, 41, 1, 1, 2),
        sample(1, 100, 41, 1, 1, 1, 302), sample(1, 100, 41, 1, 1, 1, 301, 2560),
        sample(1, 99), {previous.key, 100, 99, 301, 902, 1, SourceValidity::Valid, descriptor(), settings()},
        {previous.key, 100, 99, 301, 901, 1, SourceValidity::Valid, descriptor(), settings(1, .25f)}}};
    for (const auto& input : mutatedInputs) {
        const auto ready = complete({expected.key, input, 101});
        const auto selected = select_for_tick(epochs, Phase::Delayed, current, &previous, &expected, &ready);
        expect(selected.kind == OutputKind::DelayedRaw && selected.reason == Reason::KeyMismatch,
               "same key with different immutable inputs admitted");
    }
    struct InvalidCase { const IngressSample input; const Reason reason; };
    const std::array<InvalidCase, 11> invalid{{
        {sample(0, 101), Reason::MissingIdentity}, {sample(2, 0), Reason::MissingIdentity},
        {sample(2, 101, 0), Reason::MissingIdentity}, {sample(2, 101, 41, 2), Reason::OldEpoch},
        {sample(2, 101, 41, 1, 2), Reason::KeyMismatch}, {sample(2, 101, 41, 1, 1, 0), Reason::InvalidSettings},
        {sample(2, 101, 41, 1, 1, 1, 0), Reason::MissingIdentity},
        {sample(2, 101, 41, 1, 1, 1, 301, 0), Reason::InvalidDescriptor},
        {sample(2, 101, 41, 1, 1, 1, 301, 1920, SourceValidity::Invalid), Reason::InvalidSource},
        {sample(2, 101, 41, 1, 1, 1, 301, 1920, SourceValidity::Unknown), Reason::UnknownValidity},
        {{current.key, 101, 100, 301, 0, 1, SourceValidity::Valid, descriptor(), settings()}, Reason::MissingIdentity}}};
    for (const auto& test : invalid) {
        const auto selected = select_for_tick(epochs, Phase::Delayed, test.input, &previous, &expected, &good);
        expect(selected.kind == OutputKind::CurrentSource && !selected.frame && selected.reason == test.reason &&
               selected.resetTemporal, "invalid boundary accepted as a delayed frame");
        if (print) std::printf("rejected boundary reason=%u\n", static_cast<unsigned>(test.reason));
    }
    const std::array<IngressSample, 6> incompatiblePrevious{{sample(1, 100, 42), sample(1, 100, 41, 2),
        sample(1, 100, 41, 1, 1, 2), sample(1, 100, 41, 1, 1, 1, 302),
        sample(1, 100, 41, 1, 1, 1, 301, 2560), sample(2, 100)}};
    for (const auto& old : incompatiblePrevious)
        expect(!select_for_tick(epochs, Phase::Delayed, current, &old, &expected, &good).frame,
               "old epoch/settings/source/extent or nonprevious sample accepted");
    expect(select_for_tick(epochs, Phase::Delayed, current).reason == Reason::MissingPrevious, "missing raw not explicit");
    const auto skippedTick = sample(2, 103);
    expect(!select_for_tick(epochs, Phase::Delayed, skippedTick, &previous, &expected, &good).frame,
           "last successful capture substituted for exact preceding OBS tick");
    const IngressSample disabled{current.key, 101, 100, 301, 901, 1, SourceValidity::Valid, descriptor(), settings(2, .75f, false)};
    const auto off = select_for_tick(epochs, Phase::Delayed, disabled, &previous, &expected, &good);
    expect(off.kind == OutputKind::CurrentSource && off.status == Status::Disabled && !off.job,
           "disabled filter did not return normal source drawing");
    const auto failed = complete(expected, PublicationState::Completed, true, true, false);
    const auto error = select_for_tick(epochs, Phase::Delayed, current, &previous, &expected, &failed);
    expect(error.kind == OutputKind::DelayedRaw && error.status == Status::Error && error.resetTemporal,
           "handled error did not preserve raw phase");
    for (const auto& notReady : {complete(expected, PublicationState::Completed, false),
                               complete(expected, PublicationState::Completed, true, false)})
        expect(select_for_tick(epochs, Phase::Delayed, current, &previous, &expected, &notReady).kind == OutputKind::DelayedRaw,
               "unsignalled producer/worker published");
}
void instance_and_format_boundaries(bool print)
{
    // A different instance is reconstructed in EXACTLY the same object storage.
    std::optional<PerInstanceTick> object;
    object.emplace(41);
    const auto* oldAddress = &*object;
    object.reset();
    object.emplace(42);
    expect(&*object == oldAddress, "address-reuse fixture failed");
    const Epochs other{42, 1, 1, 1};
    const auto current = sample(2, 101, 42);
    const auto previous = sample(1, 100, 42);
    const auto expected = job(previous, 1, 7, 101);
    const auto stale = complete(job(sample(1, 100), 1, 7, 101));
    const auto selected = select_for_tick(other, Phase::Delayed, current, &previous, &expected, &stale);
    expect(selected.kind == OutputKind::DelayedRaw && selected.frame == previous.key,
           "address reuse resurrected destroyed instance output");
    PerInstanceTick first(41);
    expect(first.claim(101) == TickAction::CaptureOnce && object->claim(101) == TickAction::CaptureOnce,
           "arbitrary single-instance restriction");
    expect(first.claim(101) == TickAction::AwaitLatch, "unlatched repeat recaptured");
    expect(first.latch(101, select_for_tick({41, 1, 1, 1}, Phase::Priming, sample(2, 101))) &&
           object->latch(101, selected), "two instance latches interfere");
    expect(first.peek(101)->frame->instanceId == 41 && object->peek(101)->frame->instanceId == 42,
           "cross-instance view content");
    PerInstanceTick invalid(0);
    expect(invalid.claim(101) == TickAction::Reject, "absent instance identity admitted");
    expect(object->claim(102) == TickAction::CaptureOnce && !object->latch(102, *first.peek(101)),
           "foreign instance selection latched");
    expect(!object->latch(102, selected), "old selection latched into a new canonical tick");
    const auto good = descriptor();
    expect(good.valid(), "SDR descriptor rejected");
    expect(FrameDescriptor{{2560, 1440}, {0, 0, 2560, 1440}, PixelFormat::Rgba16Float,
           ColorSpace::ScRgbLinear, AlphaMode::Straight, 203}.valid(), "HDR scRGB rejected");
    expect(FrameDescriptor{{2560, 1440}, {0, 0, 2560, 1440}, PixelFormat::Rgb10A2,
           ColorSpace::Rec2020Pq, AlphaMode::Opaque, 203}.valid(), "HDR PQ rejected");
    expect(FrameDescriptor{{2560, 1440}, {0, 0, 2560, 1440}, PixelFormat::Rgba16Float,
           ColorSpace::Rec2020Linear, AlphaMode::Premultiplied, 203}.valid(), "linear Rec2020 rejected");
    const std::array<FrameDescriptor, 8> invalidFormats{{
        {{1920, 1080}, {1910, 0, 11, 1080}, good.format, good.color, good.alpha, 80},
        {{1920, 1080}, {0, 0, 0, 1080}, good.format, good.color, good.alpha, 80},
        {{1920, 1080}, {std::numeric_limits<uint32_t>::max(), 0, 2, 1}, good.format, good.color, good.alpha, 80},
        {good.allocation, good.active, PixelFormat::Unknown, good.color, good.alpha, 80},
        {good.allocation, good.active, good.format, ColorSpace::Rec2020Pq, good.alpha, 80},
        {good.allocation, good.active, good.format, good.color, AlphaMode::Unknown, 80},
        {good.allocation, good.active, good.format, good.color, good.alpha, 0},
        {good.allocation, good.active, good.format, good.color, good.alpha, std::numeric_limits<float>::infinity()}}};
    for (const auto& value : invalidFormats) expect(!value.valid(), "impossible descriptor admitted");
    expect(FrameDescriptor{{1920, 1080}, {10, 20, 1800, 1000}, PixelFormat::Rgba8,
           ColorSpace::Rec709Srgb, AlphaMode::Straight, 80}.valid(), "valid active rectangle rejected");
    if (print) std::puts("manual two-instance/address reuse and SDR/HDR descriptor boundaries: PASS");
}
void epochs_and_publication()
{
    const Epochs before{41, 7, 11, 3};
    struct ResetCase { ResetCause cause; uint64_t content, history, generation; bool discard; };
    const std::array<ResetCase, 11> cases{{
        {ResetCause::SourceChanged, 8, 12, 3, true}, {ResetCause::Cut, 8, 12, 3, true},
        {ResetCause::InvalidInput, 8, 12, 3, true}, {ResetCause::SettingsChanged, 8, 12, 4, true},
        {ResetCause::ExtentChanged, 8, 12, 4, true}, {ResetCause::FormatChanged, 8, 12, 4, true},
        {ResetCause::DeviceChanged, 8, 12, 4, true}, {ResetCause::Disabled, 8, 12, 3, true},
        {ResetCause::EvaluationSkipped, 7, 12, 3, false}, {ResetCause::EvaluationFailure, 7, 12, 3, false},
        {ResetCause::DeadlineMiss, 7, 12, 3, false}}};
    for (const auto& test : cases) {
        const auto changed = reset_epochs(before, test.cause);
        expect(changed && changed->next == Epochs{41, test.content, test.history, test.generation} &&
               changed->resetNr && changed->resetOfa && changed->discardDelayed == test.discard,
               "independently stated epoch/reset oracle mismatch");
        const auto current = sample(12, 200, 41, test.content, test.generation);
        const auto previous = sample(11, 199, 41, test.content, test.generation);
        const auto fresh = job(previous, test.history, 8, 200);
        const auto stale = complete(job(sample(11, 199, 41, 7, 3), 11, 8, 200));
        expect(select_for_tick(changed->next, Phase::Delayed, current, &previous, &fresh, &stale).kind == OutputKind::DelayedRaw,
               "completion before joint reset resurrected");
    }
    const auto max = std::numeric_limits<uint64_t>::max();
    expect(!reset_epochs({41, 1, max, 1}, ResetCause::DeadlineMiss), "history epoch wrapped");
    expect(!reset_epochs({41, max, 1, 1}, ResetCause::Cut), "content epoch wrapped");
    expect(!reset_epochs({41, 1, 1, max}, ResetCause::DeviceChanged), "resource generation wrapped");
    using P = PublicationState;
    // Columns Complete, Publish, Expire, Cancel. Terminal rows have no valid transition.
    const std::array<std::array<std::optional<P>, 4>, 5> oracle{{
        {P::Completed, std::nullopt, P::Expired, P::Cancelled},
        {std::nullopt, P::Published, P::Expired, P::Cancelled},
        {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
        {std::nullopt, std::nullopt, std::nullopt, std::nullopt}}};
    const std::array<P, 5> states{P::Pending, P::Completed, P::Published, P::Expired, P::Cancelled};
    const std::array<PublicationEvent, 4> events{PublicationEvent::Complete, PublicationEvent::Publish,
        PublicationEvent::Expire, PublicationEvent::Cancel};
    for (size_t s = 0; s < states.size(); ++s) for (size_t e = 0; e < events.size(); ++e)
        expect(advance_publication(states[s], events[e]) == oracle[s][e], "terminal publication revived");
    const auto current = sample(2, 101);
    const auto previous = sample(1, 100);
    const auto submitted = job(previous, 1, 7, 101);
    for (auto state : {P::Expired, P::Cancelled, P::Published}) {
        const auto late = complete(submitted, state);
        const auto selected = select_for_tick({41, 1, 1, 1}, Phase::Delayed, current, &previous, &submitted, &late);
        expect(selected.kind == OutputKind::DelayedRaw && selected.frame == previous.key, "terminal result re-published");
    }
    const auto lateDeadline = job(previous, 1, 7, 100);
    const auto late = complete(lateDeadline);
    expect(select_for_tick({41, 1, 1, 1}, Phase::Delayed, current, &previous, &lateDeadline, &late).kind == OutputKind::DelayedRaw,
           "post-deadline result published in a later tick");
}
void bounded_ownership()
{
    // Repeated valid pixels have equal content but DISTINCT OBS ingress keys.
    const std::array<uint8_t, 4> heldPixels{17, 33, 91, 128};
    const auto againPixels = heldPixels;
    const auto held = sample(1, 100);
    const auto again = sample(2, 101);
    const auto heldJob = job(held, 1, 7, 101);
    const auto heldReady = complete(heldJob);
    expect(heldPixels == againPixels && held.key != again.key &&
           select_for_tick({41, 1, 1, 1}, Phase::Delayed, again, &held, &heldJob, &heldReady).kind == OutputKind::DelayedNr,
           "repeated pixels incorrectly treated as physical no-signal or duplicate ingress");
    const auto owner = job(sample(1, 100), 1, 7, 101).key;
    // Explicit completion oracle: producer and consumer must finish; an unsubmitted
    // recording must be discarded, a submitted recording must actually finish.
    const SlotOwnership empty{std::nullopt, false, true, true, true, true};
    const SlotOwnership running{owner, true, true, true, false, true};
    const SlotOwnership consumedLater{owner, true, true, true, true, false};
    const SlotOwnership finished{owner, true, true, true, true, true};
    const SlotOwnership unsent{owner, false, false, true, false, true};
    const SlotOwnership discarded{owner, false, true, true, false, true};
    const SlotOwnership producing{owner, true, true, false, true, true};
    expect(can_reuse(empty) && can_reuse(finished) && can_reuse(discarded), "completed ownership retained");
    expect(!can_reuse(running) && !can_reuse(consumedLater) && !can_reuse(unsent) && !can_reuse(producing),
           "live producer/worker/consumer/recording ownership released");
    const std::array<SlotOwnership, 3> full{running, consumedLater, unsent};
    expect(!admit_slot(full), "three saturated slots admitted overwrite or slot4");
    expect(admit_slot(std::array<SlotOwnership, 3>{running, finished, consumedLater}) == 1,
           "exact completed slot not admitted");
    expect(admit_slot(std::array<SlotOwnership, 3>{empty, running, consumedLater}) == 0,
           "empty slot not admitted");
    expect(!admit_slot(std::array<SlotOwnership, 4>{empty, empty, empty, empty}), "unbounded ring admitted");
    for (auto event : {PublicationEvent::Expire, PublicationEvent::Cancel}) {
        expect(advance_publication(PublicationState::Pending, event).has_value(), "expiry/cancel not represented");
        expect(!can_reuse(running) && !can_reuse(consumedLater), "expiry/cancel released live submission");
    }
    const std::array<RetiredGeneration, 2> retired{{{1, full, false, true}, {2, full, false, true}}};
    expect(admit_retirement(1, retired) == RetirementAction::AlreadyRetired, "retirement not idempotent");
    expect(admit_retirement(3, retired) == RetirementAction::BudgetExhausted, "third retired generation admitted");
    expect(!can_collect(retired[0]) && !can_collect(retired[1]), "unresolved generation collected");
    const std::array<SlotOwnership, 3> done{finished, empty, discarded};
    expect(!can_collect({1, done, false, true}) && !can_collect({1, done, true, false}),
           "feature creation or undiscarded recording freed");
    expect(!can_collect({1, {finished, empty, consumedLater}, true, true}), "retired consumer freed before completion");
    expect(can_collect({1, done, true, true}), "completed generation not collectable");
    expect(admit_retirement(3, std::span<const RetiredGeneration>(retired.data() + 1, 1)) == RetirementAction::Retire,
           "reclaimed retirement budget unavailable");
    expect(admit_retirement(0, retired) == RetirementAction::Reject, "missing generation identity admitted");
}

void bad_control(std::string_view control)
{
    const Epochs epochs{41, 1, 1, 1};
    if (control == "old-key") {
        const auto current = sample(2, 101);
        const auto previous = sample(1, 100);
        const auto expected = job(previous, 1, 7, 101);
        const auto stale = complete(job(sample(1, 100, 42), 1, 7, 101));
        expect(select_for_tick(epochs, Phase::Delayed, current, &previous, &expected, &stale).kind == OutputKind::DelayedNr,
               "BAD CONTROL rejected: foreign-instance old key was not admitted");
    } else if (control == "invalid-descriptor") {
        expect(select_for_tick(epochs, Phase::Priming, sample(1, 100, 41, 1, 1, 1, 301, 0)).kind == OutputKind::PrimingRaw,
               "BAD CONTROL rejected: zero extent was not admitted");
    } else throw std::runtime_error("unknown bad control");
}
} // namespace

void held_nr_frames()
{
    const Selection late{101, OutputKind::DelayedRaw, Status::Bypassed, Reason::DeadlineMiss, std::nullopt, std::nullopt, false};
    expect(hold_previous_nr(late, true, 100, 0) && hold_previous_nr(late, true, 100, kMaxHeldNrFrames - 1),
           "late job did not re-show the previous NR frame");
    expect(!hold_previous_nr(late, true, 100, kMaxHeldNrFrames), "NR hold is not bounded");
    expect(!hold_previous_nr(late, false, 100, 0), "held a frame the output never showed as NR");
    expect(!hold_previous_nr(late, true, 99, 0), "held an NR frame from a non-adjacent tick");
    const Selection reset{101, OutputKind::DelayedRaw, Status::Bypassed, Reason::DeadlineMiss, std::nullopt, std::nullopt, true};
    const Selection failed{101, OutputKind::DelayedRaw, Status::Error, Reason::EvaluationFailure, std::nullopt, std::nullopt, true};
    const Selection expired{101, OutputKind::DelayedRaw, Status::Bypassed, Reason::Expired, std::nullopt, std::nullopt, true};
    const Selection applied{101, OutputKind::DelayedNr, Status::Applied, Reason::None, std::nullopt, std::nullopt, false};
    expect(!hold_previous_nr(reset, true, 100, 0) && !hold_previous_nr(failed, true, 100, 0) &&
           !hold_previous_nr(expired, true, 100, 0) && !hold_previous_nr(applied, true, 100, 0), "held outside a plain late job");
    const auto current = sample(2, 101);
    const auto previous = sample(1, 100);
    const auto unseen = select_for_tick({41, 1, 1, 1}, Phase::Delayed, current, &previous);
    expect(unseen.kind == OutputKind::DelayedRaw && unseen.reason == Reason::DeadlineMiss && unseen.resetTemporal,
           "a frame the model never received kept the temporal history");
}

int main(int argc, char** argv)
{
    try {
        if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--manual")) {
            immutable_settings();
            known_sequence(argc == 2);
            adversarial(argc == 2);
            instance_and_format_boundaries(argc == 2);
            epochs_and_publication();
            bounded_ownership();
            held_nr_frames();
            std::printf("PASS frame_contract: %u checks\n", checks);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--bad-control") {
            bad_control(argv[2]);
            std::fprintf(stderr, "FAIL: invalid bad control unexpectedly admitted\n");
            return 2;
        }
        throw std::runtime_error("unknown invocation");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL frame_contract: %s (%u checks)\n", error.what(), checks);
        return 1;
    }
}
