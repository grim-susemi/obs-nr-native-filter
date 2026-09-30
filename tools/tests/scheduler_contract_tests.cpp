// SPDX-License-Identifier: GPL-3.0-only
#include "frame_pipeline.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace nr;
using namespace nr::pipeline;
namespace {
unsigned checks = 0;
unsigned activeSessions = 0;
void expect(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
void await(const std::shared_future<void>& signal)
{
    expect(signal.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "exact completion event timed out");
    signal.get();
}
Settings settings(uint64_t revision = 1, bool enabled = true)
{
    return {{revision}, enabled, "C:/owned/not-loaded-runtime.dll", {.75f}, {0, 0, 1, 1, 0, -1, true}};
}
IngressSample sample(const Epochs& e, uint64_t sequence, uint64_t tick, uint64_t source = 301,
                     SourceValidity valid = SourceValidity::Valid, uint64_t revision = 1, bool enabled = true)
{
    return {{e.instanceId, e.contentEpoch, sequence}, tick, tick - 1, source, 901, e.resourceGeneration, valid,
        {{1920, 1080}, {0, 0, 1920, 1080}, PixelFormat::Bgra8, ColorSpace::Rec709Srgb, AlphaMode::Straight, 80}, settings(revision, enabled)};
}
struct Image {
    const uint64_t content;
    const std::array<uint8_t, 4> pixels;
};
std::shared_ptr<const RawFrame> snapshot(const IngressSample& input, uint64_t id)
{
    return std::make_shared<const RawFrame>(RawFrame{input,
        std::make_shared<const Image>(Image{id, {static_cast<uint8_t>(id + 11), 29, 47, 191}})});
}
const Image& decoded(const std::shared_ptr<const void>& image)
{
    expect(static_cast<bool>(image), "selected actual image missing");
    return *std::static_pointer_cast<const Image>(image);
}
JobContract changed(const JobContract& j, unsigned mutation)
{
    const auto& k = j.key;
    const JobKey key{{k.frame.instanceId + (mutation == 1), k.frame.contentEpoch + (mutation == 2),
        k.frame.ingressSequence + (mutation == 3)}, k.historyEpoch + (mutation == 4),
        k.resourceGeneration + (mutation == 5), k.submissionSerial + (mutation == 6)};
    const auto& i = j.input;
    const IngressSample input{i.key, i.canonicalTick, i.precedingCanonicalTick, i.sourceIdentity, i.deviceIdentity,
        i.resourceGeneration, i.validity, i.descriptor, mutation == 7 ? settings(2) : i.settings};
    return {key, input, j.deadlineTick + (mutation == 8)};
}
struct Gate {
    std::promise<void> entered, dependency;
    const std::shared_future<void> started = entered.get_future().share();
    const std::shared_future<void> producerDependency = dependency.get_future().share();
    const unsigned mutation;
    const bool evaluated, wrongContent, throws;
    bool released = false;
    Gate(unsigned m = 0, bool evaluatedValue = true, bool badContent = false, bool throwValue = false)
        : mutation(m), evaluated(evaluatedValue), wrongContent(badContent), throws(throwValue) {}
    void release() { if (!released) { released = true; dependency.set_value(); } }
};
class CpuProcessor final : public Processor {
public:
    std::atomic<unsigned> calls{0};
    Processed process(const Work& work, std::stop_token) override
    {
        ++calls;
        auto gate = std::static_pointer_cast<Gate>(work.binding.processorLease);
        gate->entered.set_value();
        if (gate->producerDependency.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            throw std::runtime_error("controlled producer dependency not delivered");
        gate->producerDependency.get();
        if (gate->throws) throw std::runtime_error("explicit handled processor failure");
        const auto& original = *std::static_pointer_cast<const Image>(work.raw->image);
        return {changed(work.job, gate->mutation), gate->evaluated,
            std::make_shared<const Image>(Image{original.content + (gate->wrongContent ? 1 : 0),
                {original.pixels[0], 31, 49, original.pixels[3]}})};
    }
};
interop::Ownership physical(const Admission& a, unsigned ready = 3, bool open = false,
                             uint64_t consumer = 0, uint64_t consumerDone = 0)
{
    return {a.job.key, interop::WorkPhase::Submitted, 1, 1, consumer,
        (ready & 1) ? 1u : 0u, (ready & 2) ? 1u : 0u, consumerDone, open, false, false};
}
struct Record { const Admission admission; const std::shared_ptr<Gate> gate; };
struct Session {
    std::shared_ptr<CpuProcessor> processor = std::make_shared<CpuProcessor>();
    FramePipeline pipeline;
    std::vector<Record> records;
    bool closed = false;
    explicit Session(uint64_t instance = 41) : pipeline({instance, 1, 1, 1}, processor) { ++activeSessions; }
    ~Session() { if (!closed) { try { close(); } catch (const std::exception& e) { std::fprintf(stderr, "CLEANUP FAIL %s\n", e.what()); std::terminate(); } } }
    TickPlan submit(const IngressSample& input, uint64_t id, std::shared_ptr<Gate> gate = std::make_shared<Gate>())
    {
        const auto plan = pipeline.begin(input, input.canonicalTick + 1);
        expect(plan.action == TickAction::CaptureOnce, "new tick did not own capture");
        auto raw = snapshot(input, id);
        if (plan.admission) {
            const auto& a = *plan.admission;
            records.push_back({a, gate}); // Register exact completion/cleanup BEFORE dispatch.
            auto captured = physical(a, 0); captured.phase = interop::WorkPhase::Captured; captured.worker = 0;
            const WorkBinding binding{{a.job.key, a.slot}, captured, {}, gate};
            expect(pipeline.finish(raw, &binding), "exact raw/captured transport dispatch refused");
        } else expect(pipeline.finish(raw), "admission skip lost independent raw capture");
        return plan;
    }
    void complete(size_t index, unsigned ready = 3)
    {
        auto& r = records.at(index);
        await(r.gate->started); r.gate->release(); await(r.admission.returned);
        expect(pipeline.observe({r.admission.job.key, r.admission.slot}, physical(r.admission, ready)), "physical receipt refused");
    }
    void close()
    {
        for (auto& r : records) r.gate->release();
        for (auto& r : records) {
            await(r.admission.returned);
            if (const auto state = pipeline.state(r.admission.job.key)) {
                auto done = physical(r.admission);
                done.consumer = state->physical.consumer;
                done.consumerDone = done.consumer;
                expect(pipeline.observe(state->ticket, done), "cleanup exact physical completion refused");
            }
        }
        const auto exited = pipeline.worker_exit();
        pipeline.request_stop(); await(exited);
        expect(pipeline.try_close() && pipeline.try_close(), "resolved worker/leases did not close exactly once");
        closed = true;
        --activeSessions;
    }
};
void selection(const FramePipeline& p, uint64_t tick, uint64_t id, OutputKind kind, bool print = false)
{
    const auto* selected = p.view(tick);
    expect(selected && selected->selection.kind == kind, "raw/NR phase oracle mismatch");
    const auto& image = decoded(selected->image);
    expect(image.content == id, "independent image-decoded source-content oracle mismatch");
    expect(selected->raw && selected->selection.frame == selected->raw->input.key, "selection lost immutable raw key");
    const auto& original = decoded(selected->raw->image);
    expect(original.content == id && original.pixels[3] == image.pixels[3], "CPU processing changed lineage/alpha");
    if (kind != OutputKind::DelayedNr) expect(image.pixels == original.pixels, "fallback was not untouched same-phase raw");
    if (print) {
        const auto& k = *selected->selection.frame;
        std::printf("tick=%llu selected_content=%llu kind=%u frame=%llu/%llu/%llu",
            tick, image.content, static_cast<unsigned>(kind), k.instanceId, k.contentEpoch, k.ingressSequence);
        if (selected->selection.job) {
            const auto& j = *selected->selection.job;
            std::printf(" job_history=%llu generation=%llu submission=%llu", j.historyEpoch, j.resourceGeneration, j.submissionSerial);
        }
        std::printf(" reason=%u reset=%u\n", static_cast<unsigned>(selected->selection.reason), selected->selection.resetTemporal);
    }
}
void sequence(bool late, bool print, bool badContent = false, bool resurrect = false)
{
    Session s;
    s.submit(sample(s.pipeline.epochs(), 1, 100), 0, std::make_shared<Gate>(0, true, badContent));
    selection(s.pipeline, 100, 0, OutputKind::PrimingRaw, print);
    s.complete(0);
    for (uint64_t tick = 101; tick <= 105; ++tick) {
        const uint64_t id = tick - 100;
        s.submit(sample(s.pipeline.epochs(), id + 1, tick), id);
        const auto kind = late && tick == 102 ? OutputKind::DelayedRaw : OutputKind::DelayedNr;
        selection(s.pipeline, tick, id - 1, kind, print);
        if (late && tick == 101) { await(s.records[1].gate->started); continue; }
        if (late && tick == 102) {
            const auto key = s.records[1].admission.job.key;
            expect(s.pipeline.state(key)->publication == PublicationState::Expired, "deadline miss not irreversible");
            s.complete(1);
            expect(s.pipeline.state(key)->publication == PublicationState::Expired, "late physical completion resurrected publication");
            if (print) {
                const auto state = s.pipeline.state(key);
                std::printf("publication=Expired dispatchReturned=%u producerDone=%llu workerDone=%llu consumerOpen=%u\n",
                    state->dispatchReturned, state->physical.producerDone, state->physical.workerDone, state->physical.consumerOpen);
            }
            selection(s.pipeline, tick, id - 1, resurrect ? OutputKind::DelayedNr : OutputKind::DelayedRaw, print);
        }
        s.complete(static_cast<size_t>(id));
        const auto before = s.pipeline.counters();
        for (unsigned view = 0; view < 4; ++view) {
            const auto duplicate = s.pipeline.begin(sample(s.pipeline.epochs(), id + 1, tick), tick + 1);
            expect(duplicate.action == TickAction::ReuseLatch, "repeated view recaptured");
            selection(s.pipeline, tick, id - 1, kind);
        }
        const auto after = s.pipeline.counters();
        expect(before.captures == after.captures && before.dispatches == after.dispatches && before.selections == after.selections,
            "views advanced capture/evaluation/selection");
    }
    expect(s.pipeline.counters().captures == 6 && s.pipeline.counters().dispatches == 6 && s.pipeline.counters().selections == 6,
        "one capture/dispatch/selection per canonical tick violated");
    s.close();
    if (print) std::puts("steady_selected_sequence=0,1,2,3,4 CPU_only=true cleanup=resolved");
}
void exhaustive()
{
    const std::array<std::array<unsigned, 3>, 6> permutations{{{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}}};
    for (const auto& order : permutations) for (unsigned mask = 0; mask < 4; ++mask)
        for (bool late : {false, true}) for (unsigned mutation = 0; mutation <= 8; ++mutation) for (bool evaluated : {false, true}) {
            Session s;
            auto gate = std::make_shared<Gate>(mutation, evaluated);
            const auto first = s.submit(sample(s.pipeline.epochs(), 1, 100), 0, gate);
            expect(first.admission && first.admission->slot == 0, "first bounded slot not zero");
            await(gate->started);
            if (!late) s.complete(0, mask);
            const bool nr = !late && mask == 3 && mutation == 0 && evaluated;
            const auto kind = nr ? OutputKind::DelayedNr : OutputKind::DelayedRaw;
            const auto current = sample(s.pipeline.epochs(), 2, 101);
            s.submit(current, 1);
            for (unsigned view : order) {
                (void)view;
                selection(s.pipeline, 101, 0, kind);
                if (late && !gate->released) s.complete(0, mask); // Exact completion BETWEEN views.
                const auto repeated = s.pipeline.begin(current, 102);
                expect(repeated.action == TickAction::ReuseLatch && repeated.selection.kind == kind, "view order altered deadline latch");
            }
            const auto state = s.pipeline.state(first.admission->job.key);
            if (!nr && state) expect(state->publication == PublicationState::Expired, "reordered result revived expired slot");
            expect(s.pipeline.begin(sample(s.pipeline.epochs(), 1, 99), 100).action == TickAction::Reject, "reordered old canonical tick admitted");
            selection(s.pipeline, 101, 0, kind);
        }
    std::puts("exhaustive=864 readiness/key/immutable-input/evaluation/deadline/view-order combinations");
}
void saturation_and_lifetime()
{
    Session s;
    for (uint64_t n = 0; n < 6; ++n) {
        const auto plan = s.submit(sample(s.pipeline.epochs(), n + 1, 100 + n), n);
        if (n == 0) await(s.records[0].gate->started); // Foreign call is held BEFORE render operations below.
        if (n < 3) expect(plan.admission && plan.admission->slot == n, "saturated ring reused unfinished slot");
        else expect(!plan.admission && plan.admissionReason == Reason::SlotsFull, "slot4/unbounded admission allowed");
        selection(s.pipeline, 100 + n, n == 0 ? 0 : n - 1, n == 0 ? OutputKind::PrimingRaw : OutputKind::DelayedRaw);
    }
    expect(s.pipeline.counters().dispatches == 3 && s.pipeline.counters().captures == 6, "saturation lost independent raw history");
    const auto& first = s.records[0];
    const auto key = first.admission.job.key;
    std::weak_ptr<void> lifetime = first.gate;
    expect(s.pipeline.cancel(key) && s.pipeline.cancel(key), "repeat cancel not idempotent");
    const auto state = s.pipeline.state(key);
    expect(state && !state->dispatchReturned && !interop::reusable(state->physical) && !lifetime.expired(), "cancel released foreign/producer lifetime");
    s.complete(0);
    auto open = physical(first.admission, 3, true, 1, 0);
    expect(s.pipeline.observe(state->ticket, open), "open consumer receipt rejected");
    s.pipeline.collect();
    expect(s.pipeline.state(key).has_value(), "open consumer lease was reused before signal");
    open.consumerOpen = false;
    expect(s.pipeline.observe(state->ticket, open), "consumer submission receipt rejected");
    s.pipeline.collect();
    expect(s.pipeline.state(key).has_value(), "pending consumer completion was released");
    open.consumerDone = 1;
    expect(s.pipeline.observe(state->ticket, open), "exact consumer completion rejected");
    s.pipeline.collect();
    expect(!s.pipeline.state(key) && !s.pipeline.observe(state->ticket, open), "slot did not retire exactly once");
    const auto exited = s.pipeline.worker_exit();
    s.pipeline.request_stop(); s.pipeline.request_stop();
    expect(!s.pipeline.try_close(), "shutdown force-freed queued/held ownership");
    for (size_t n = 1; n < s.records.size(); ++n) s.complete(n);
    await(exited);
    s.close();
    std::puts("saturation=3 raw_history=independent expiry_ne_physical=true consumer_open/pending/exact_retirement=PASS");
}
void generations_and_resets()
{
    const std::array<ResetCause, 11> causes{ResetCause::SourceChanged, ResetCause::Cut, ResetCause::InvalidInput,
        ResetCause::SettingsChanged, ResetCause::ExtentChanged, ResetCause::FormatChanged, ResetCause::DeviceChanged,
        ResetCause::Disabled, ResetCause::EvaluationSkipped, ResetCause::EvaluationFailure, ResetCause::DeadlineMiss};
    for (auto cause : causes) {
        Session s;
        const auto first = s.submit(sample(s.pipeline.epochs(), 1, 100), 0);
        await(s.records[0].gate->started);
        expect(s.pipeline.reset(cause), "supported reset not admitted");
        const auto changedEpoch = s.pipeline.epochs();
        const bool discard = cause != ResetCause::EvaluationSkipped && cause != ResetCause::EvaluationFailure && cause != ResetCause::DeadlineMiss;
        expect(changedEpoch.historyEpoch == 2 && changedEpoch.contentEpoch == (discard ? 2u : 1u), "joint temporal reset epoch wrong");
        s.complete(0);
        expect(s.pipeline.state(first.admission->job.key)->publication == PublicationState::Cancelled, "reset late completion resurrected");
        s.submit(sample(s.pipeline.epochs(), 2, 101), 1);
        selection(s.pipeline, 101, discard ? 1 : 0, discard ? OutputKind::PrimingRaw : OutputKind::DelayedRaw);
    }
    Session s;
    std::array<std::shared_ptr<void>, 3> leases{std::make_shared<int>(1), std::make_shared<int>(2), std::make_shared<int>(3)};
    std::array<std::weak_ptr<void>, 3> weak{leases[0], leases[1], leases[2]};
    for (uint64_t generation = 1; generation <= 3; ++generation) {
        expect(s.pipeline.hold_generation(generation, leases[generation - 1]), "feature generation lease not retained");
        leases[generation - 1].reset();
        if (generation < 3) expect(s.pipeline.reset(ResetCause::SettingsChanged), "first two retired generations refused");
    }
    const auto before = s.pipeline.epochs();
    expect(!s.pipeline.reset(ResetCause::SettingsChanged) && s.pipeline.epochs() == before && s.pipeline.retired_generations() == 2,
        "retirement budget advanced/allocated generation4");
    const auto plan = s.submit(sample(before, 1, 100), 0);
    expect(!plan.admission && plan.admissionReason == Reason::RetirementBudget, "budget exhaustion not explicit admission bypass");
    for (const auto& lifetime : weak) expect(!lifetime.expired(), "unresolved feature generation freed");
    expect(!s.pipeline.complete_generation(4), "foreign generation completion admitted");
    expect(s.pipeline.complete_generation(1), "exact retired feature completion refused");
    s.pipeline.collect();
    expect(weak[0].expired() && s.pipeline.retired_generations() == 1, "resolved generation budget not reclaimed");
    expect(s.pipeline.reset(ResetCause::SettingsChanged), "reclaimed budget not resumable");
    expect(s.pipeline.complete_generation(2) && s.pipeline.complete_generation(3), "remaining exact feature completions refused");
    s.close();
    expect(weak[1].expired() && weak[2].expired(), "generation leases survived resolved close");
    std::puts("resets=11 retired_generations_max=2 exhaustion/resume/exact_feature_retirement=PASS");
}
void boundaries_and_interruptions()
{
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        Session s;
        s.submit(sample(s.pipeline.epochs(), 1, 100), 0); s.complete(0);
        const auto e = s.pipeline.epochs();
        const auto base = sample(e, 2, 101, invalid == 0 ? 0 : 301,
            invalid == 1 ? SourceValidity::Invalid : invalid == 2 ? SourceValidity::Unknown : SourceValidity::Valid,
            invalid == 3 ? 0 : 1, invalid != 4);
        const IngressSample input{{invalid == 5 ? 42u : base.key.instanceId, base.key.contentEpoch, base.key.ingressSequence},
            base.canonicalTick, invalid == 6 ? 101 : base.precedingCanonicalTick, base.sourceIdentity, base.deviceIdentity,
            base.resourceGeneration, base.validity,
            invalid == 7 ? FrameDescriptor{{0,1080},{0,0,0,1080},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Straight,80} : base.descriptor,
            base.settings};
        const auto plan = s.pipeline.begin(input, 102);
        expect(plan.selection.kind == OutputKind::CurrentSource && !plan.selection.frame && !plan.admission,
            "invalid/disabled input published stale delayed eligibility");
        expect(s.pipeline.finish(nullptr), "current-source placeholder not latched");
        expect(s.pipeline.view(101) && !s.pipeline.view(101)->image, "invalid input retained delayed image");
        s.submit(sample(s.pipeline.epochs(), 3, 102), 2);
        selection(s.pipeline, 102, 2, OutputKind::PrimingRaw);
    }
    {
        Session s;
        const auto input = sample(s.pipeline.epochs(), 1, 100);
        const auto plan = s.pipeline.begin(input, 101);
        expect(plan.admission.has_value(), "binding boundary setup failed");
        expect(s.pipeline.begin(input, 101).action == TickAction::AwaitLatch, "unlatched repeated view reserved twice");
        const auto& a = *plan.admission;
        auto gate = std::make_shared<Gate>();
        auto captured = physical(a, 0); captured.phase = interop::WorkPhase::Captured; captured.worker = 0;
        const WorkBinding wrong{{changed(a.job, 6).key, a.slot}, captured, {}, gate};
        expect(!s.pipeline.finish(snapshot(input, 0), &wrong), "foreign submission ticket admitted");
        const WorkBinding exact{{a.job.key, a.slot}, captured, {}, gate};
        expect(!s.pipeline.finish(snapshot(sample(s.pipeline.epochs(), 2, 100), 0), &exact), "same address/different raw ingress admitted");
        s.records.push_back({a, gate});
        expect(s.pipeline.finish(snapshot(input, 0), &exact) && !s.pipeline.finish(snapshot(input, 0), &exact), "finish duplicated dispatch");
        s.complete(0);
        auto sentinel = physical(a); sentinel.workerDone = UINT64_MAX;
        expect(!s.pipeline.observe({a.job.key, a.slot}, sentinel), "failed fence sentinel became ready");
        expect(!s.pipeline.observe({a.job.key, 2}, physical(a)), "wrong physical slot receipt admitted");
        auto reordered = physical(a, 0);
        expect(!s.pipeline.observe({a.job.key, a.slot}, reordered), "reordered physical progress regressed");
        s.submit(sample(s.pipeline.epochs(), 2, 101), 1);
        selection(s.pipeline, 101, 0, OutputKind::DelayedRaw);
        s.complete(1);
        s.submit(sample(s.pipeline.epochs(), 3, 104), 2);
        expect(s.pipeline.view(104)->selection.kind == OutputKind::CurrentSource && !s.pipeline.view(104)->image,
            "last successful capture replaced absent canonical predecessor");
    }
    {
        std::optional<FramePipeline> storage;
        auto processor = std::make_shared<CpuProcessor>();
        storage.emplace(Epochs{41,1,1,1}, processor);
        const auto* address = &*storage;
        auto exited = storage->worker_exit(); storage->request_stop(); await(exited); expect(storage->try_close(), "empty prior instance not closed");
        storage.reset(); storage.emplace(Epochs{42,1,1,1}, processor);
        expect(&*storage == address, "actual scheduler storage-address reuse fixture failed");
        const auto rejected = storage->begin(sample({41,1,1,1}, 1, 100), 101);
        expect(!rejected.admission && !rejected.selection.frame, "source address reuse admitted old instance");
        expect(storage->finish(nullptr), "reused-instance placeholder not latched");
        exited = storage->worker_exit(); storage->request_stop(); await(exited); expect(storage->try_close(), "reused instance not closed");
    }
    {
        Session s;
        for (uint64_t n = 0; n < 20; ++n) {
            s.submit(sample(s.pipeline.epochs(), n + 1, 100 + n), n, std::make_shared<Gate>(0, true, false, n == 3));
            s.complete(static_cast<size_t>(n));
            expect(s.pipeline.reset(ResetCause::EvaluationSkipped) && s.pipeline.reset(ResetCause::DeadlineMiss), "repeated interruption reset refused");
        }
        expect(s.pipeline.counters().dispatches == 20 && s.pipeline.epochs().historyEpoch >= 41, "repeated interruptions lost serial/history progress");
    }
    {
        auto processor = std::make_shared<CpuProcessor>();
        FramePipeline p({41, 1, UINT64_MAX, 1}, processor);
        expect(!p.reset(ResetCause::DeadlineMiss), "history overflow wrapped");
        const auto input = sample(p.epochs(), 1, 100);
        const auto plan = p.begin(input, 101);
        expect(!plan.admission && plan.admissionReason == Reason::KeyMismatch, "overflow reused old temporal history for new work");
        expect(p.finish(snapshot(input, 0)), "history-exhausted raw priming lost");
        auto exited = p.worker_exit(); p.request_stop(); await(exited); expect(p.try_close(), "history-exhausted close refused");
    }
    {
        Session s;
        s.submit(sample(s.pipeline.epochs(), 1, 100), 0, std::make_shared<Gate>(0, true, false, true)); s.complete(0);
        s.submit(sample(s.pipeline.epochs(), 2, 101), 1);
        expect(s.pipeline.view(101)->selection.status == Status::Error && s.pipeline.view(101)->selection.reason == Reason::EvaluationFailure,
            "processor exception silently swallowed or applied");
        selection(s.pipeline, 101, 0, OutputKind::DelayedRaw);
    }
    std::puts("malformed/current_placeholder/canonical_gap/source_address_reuse/repeated_interruptions/handled_exception/history_overflow=PASS");
}
} // namespace
int main(int argc, char** argv)
{
    try {
        if (argc == 1) {
            sequence(false, false); sequence(true, false); exhaustive(); saturation_and_lifetime();
            generations_and_resets(); boundaries_and_interruptions();
        } else if (argc == 2 && std::string_view(argv[1]) == "happy") sequence(false, true);
        else if (argc == 2 && std::string_view(argv[1]) == "late") sequence(true, true);
        else if (argc == 3 && std::string_view(argv[1]) == "--bad-control") {
            if (std::string_view(argv[2]) == "wrong-content") sequence(false, true, true);
            else if (std::string_view(argv[2]) == "late-resurrection") sequence(true, true, false, true);
            else throw std::runtime_error("unknown bad control");
            throw std::runtime_error("bad control unexpectedly passed");
        } else throw std::runtime_error("unknown CLI invocation");
        expect(activeSessions == 0, "owned scheduler session still live");
        std::printf("PASS scheduler_contract checks=%u all_owned_workers_and_leases_closed=true CPU_only=true\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL scheduler_contract: %s checks=%u CPU_only=true cleanup_active_sessions=%u\n", e.what(), checks, activeSessions);
        return 1;
    }
}
