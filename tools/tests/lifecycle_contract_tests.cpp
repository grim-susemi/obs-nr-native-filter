// SPDX-License-Identifier: GPL-3.0-only
#include "nr_lifecycle.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace nr;
using namespace nr::pipeline;
using namespace nr::lifecycle;
namespace {
unsigned checks = 0;
unsigned activeSessions = 0;
void expect(bool value, const char* why)
{
    ++checks;
    if (!value) throw std::runtime_error(why);
}
void await(const std::shared_future<void>& signal)
{
    expect(signal.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "exact CPU event timed out");
    signal.get();
}
Settings settings(uint64_t revision = 1, bool enabled = true)
{
    return {{revision}, enabled, "C:/owned/not-loaded-runtime.dll", {.75f}, {0, 0, 1, 1, 0, -1, true}};
}
FrameDescriptor descriptor(uint32_t width = 1920, bool hdr = false)
{
    return {{width, 1080}, {0, 0, width, 1080}, hdr ? PixelFormat::Rgba16Float : PixelFormat::Bgra8,
        hdr ? ColorSpace::ScRgbLinear : ColorSpace::Rec709Srgb, AlphaMode::Straight, 80};
}
Observation observation(uint64_t tick = 100, uint64_t source = 301, uint64_t device = 901,
                        SourceValidity validity = SourceValidity::Valid, uint64_t revision = 1,
                        bool enabled = true, uint32_t width = 1920, bool hdr = false)
{
    return {tick, source, device, validity, descriptor(width, hdr), settings(revision, enabled)};
}
IngressSample sample(const Epochs& e, const Observation& o, uint64_t sequence)
{
    return {{e.instanceId, e.contentEpoch, sequence}, o.canonicalTick, o.canonicalTick - 1,
        o.sourceIdentity, o.deviceIdentity, e.resourceGeneration, o.validity, o.descriptor, o.settings};
}
struct Image { const uint64_t content; };
struct Gate {
    std::promise<void> entered, dependency;
    const std::shared_future<void> started = entered.get_future().share();
    const std::shared_future<void> producer = dependency.get_future().share();
    const bool evaluated;
    bool released = false;
    explicit Gate(bool success = true) : evaluated(success) {}
    void release() { if (!released) { released = true; dependency.set_value(); } }
};
class CpuProcessor final : public Processor {
    std::vector<std::weak_ptr<const void>> images_;
public:
    std::atomic<bool> inputsReleased{false};
    Processed process(const Work& work, std::stop_token) override
    {
        auto gate = std::static_pointer_cast<Gate>(work.binding.processorLease);
        images_.push_back(work.raw->image);
        gate->entered.set_value();
        if (gate->producer.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            throw std::runtime_error("exact controlled producer signal timed out");
        gate->producer.get();
        return {work.job, gate->evaluated, work.raw->image};
    }
    void worker_stopping() noexcept override
    {
        bool released = true;
        for (const auto& image : images_) released = released && image.expired();
        inputsReleased.store(released, std::memory_order_release);
    }
};
void close(FramePipeline& pipeline)
{
    const auto exited = pipeline.worker_exit(); // Subscribe before the action.
    pipeline.request_stop();
    await(exited);
    expect(pipeline.try_close(), "resolved CPU worker did not close");
}
interop::Ownership receipt(const Admission& a, unsigned ready = 7, bool open = false)
{
    return {a.job.key, interop::WorkPhase::Submitted, 1, 2, 3,
        (ready & 1) ? 1u : 0u, (ready & 2) ? 2u : 0u, (ready & 4) ? 3u : 0u, open, false, false};
}
struct Record { const Admission admission; const std::shared_ptr<Gate> gate; };
struct Session {
    std::shared_ptr<CpuProcessor> processor = std::make_shared<CpuProcessor>();
    FramePipeline pipeline;
    Observer observer;
    std::vector<Record> records;
    std::vector<uint64_t> generations;
    uint64_t lastTick = 99;
    bool closed = false;
    explicit Session(uint64_t id = 41) : pipeline({id, 1, 1, 1}, processor), observer(pipeline)
    {
        ++activeSessions;
        expect(!observer.prepare(observation()).placeholder, "initial configured snapshot refused");
    }
    ~Session()
    {
        if (!closed) {
            try { shutdown(); }
            catch (const std::exception& e) { std::fprintf(stderr, "CLEANUP FAIL %s\n", e.what()); std::terminate(); }
        }
    }
    TickPlan submit(const Observation& o, uint64_t sequence, uint64_t content,
                    std::shared_ptr<Gate> gate = std::make_shared<Gate>())
    {
        const auto prepared = observer.prepare(o);
        expect(!prepared.placeholder, "fixture attempted submission after explicit lifecycle refusal");
        const auto input = sample(prepared.epochs, o, sequence);
        const auto plan = pipeline.begin(input, o.canonicalTick + 1);
        lastTick = o.canonicalTick;
        auto raw = std::make_shared<const RawFrame>(RawFrame{input, std::make_shared<const Image>(Image{content})});
        if (plan.admission) {
            records.push_back({*plan.admission, gate}); // Futures registered BEFORE dispatch.
            auto captured = receipt(*plan.admission, 0, true);
            captured.phase = interop::WorkPhase::Captured;
            captured.worker = 0;
            const WorkBinding binding{{plan.admission->job.key, plan.admission->slot}, captured, {}, gate};
            expect(pipeline.finish(raw, &binding), "production dispatch refused immutable CPU binding");
        } else expect(pipeline.finish(raw), "independent raw admission skip failed");
        return plan;
    }
    void complete(size_t index, unsigned ready = 7, bool open = false)
    {
        const auto& r = records.at(index);
        await(r.gate->started);
        r.gate->release();
        await(r.admission.returned);
        expect(pipeline.observe({r.admission.job.key, r.admission.slot}, receipt(r.admission, ready, open)),
            "exact scripted CPU ownership receipt refused");
    }
    void hold(uint64_t generation, std::shared_ptr<void> lease)
    {
        expect(pipeline.hold_generation(generation, std::move(lease)), "feature/upload fixture hold refused");
        generations.push_back(generation);
    }
    void placeholder(const Observation& o, uint64_t sequence)
    {
        const auto prepared = observer.prepare(o);
        expect(prepared.placeholder && prepared.placeholder->kind == OutputKind::CurrentSource &&
            !prepared.placeholder->frame && !prepared.placeholder->job, "lifecycle refusal retained old output key");
        // These are genuine explicit invalid/disabled fixture observations.
        // No successful source sample is rewritten to manufacture invalidity.
        expect(o.validity != SourceValidity::Valid || !o.settings.enabled, "invalid placeholder fixture not explicit");
        const auto plan = pipeline.begin(sample(prepared.epochs, o, sequence), o.canonicalTick + 1);
        lastTick = o.canonicalTick;
        expect(!plan.admission && plan.selection.kind == OutputKind::CurrentSource, "production placeholder admitted stale work");
        expect(pipeline.finish(nullptr), "production current-source placeholder did not latch");
        const auto* view = pipeline.view(o.canonicalTick);
        expect(view && !view->raw && !view->image && !view->selection.frame && !view->selection.job,
            "production placeholder retained raw/model image");
    }
    void shutdown()
    {
        for (const auto& r : records) r.gate->release();
        for (const auto& r : records) {
            await(r.admission.returned);
            if (pipeline.state(r.admission.job.key)) {
                expect(pipeline.observe({r.admission.job.key, r.admission.slot}, receipt(r.admission)),
                    "fixture-owned producer/worker/ended-pin completion refused");
                expect(pipeline.cancel(r.admission.job.key), "fixture cleanup cancel refused");
            }
        }
        for (auto generation : generations) (void)pipeline.complete_generation(generation);
        // Match the accepted raw-before-session boundary, not merely process exit.
        const auto disabled = observation(lastTick + 1, 301, 901, SourceValidity::Valid, 1, false);
        (void)pipeline.begin(sample(pipeline.epochs(), disabled, 999), disabled.canonicalTick + 1);
        expect(pipeline.finish(nullptr), "disabled input-release cutoff failed");
        pipeline.collect();
        records.clear();
        close(pipeline);
        expect(processor->inputsReleased.load(std::memory_order_acquire), "raw inputs survived worker/session close");
        closed = true;
        --activeSessions;
    }
};
void selected(const FramePipeline& p, uint64_t tick, uint64_t content, OutputKind kind)
{
    const auto* view = p.view(tick);
    expect(view && view->selection.kind == kind && view->raw && view->image, "production selection phase mismatch");
    expect(std::static_pointer_cast<const Image>(view->image)->content == content, "image-decoded CPU content mismatch");
    expect(view->selection.frame == view->raw->input.key, "selected image lost immutable ingress identity");
}
void observation_transitions()
{
    struct Case { const Observation next; const ResetCause cause; const uint64_t generation; };
    const std::array<Case, 8> cases{{
        {observation(101, 302), ResetCause::SourceChanged, 1},
        {observation(101, 301, 902), ResetCause::DeviceChanged, 2},
        {observation(101, 301, 901, SourceValidity::Valid, 2), ResetCause::SettingsChanged, 2},
        {observation(101, 301, 901, SourceValidity::Valid, 1, true, 2560), ResetCause::ExtentChanged, 2},
        {observation(101, 301, 901, SourceValidity::Valid, 1, true, 1920, true), ResetCause::FormatChanged, 2},
        {observation(101, 301, 901, SourceValidity::Valid, 1, false), ResetCause::Disabled, 1},
        {{101, 301, 901, SourceValidity::Valid,
            {{1920,1080},{16,0,1904,1080},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Straight,80},settings()},
            ResetCause::ExtentChanged, 2},
        {{101, 301, 901, SourceValidity::Valid,
            {{1920,1080},{0,0,1920,1080},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Premultiplied,203},settings()},
            ResetCause::FormatChanged, 2}}};
    for (const auto& test : cases) {
        Session s;
        const auto old = s.submit(observation(), 1, 7);
        await(s.records[0].gate->started);
        const auto frozenJob = old.admission->job;
        const auto update = s.observer.prepare(test.next);
        expect(update.cause == test.cause && update.resetTemporal &&
            update.epochs == Epochs{41,2,2,test.generation}, "observable transition did not jointly reset exact epochs");
        expect(old.admission->job == frozenJob && frozenJob.key == JobKey{{41,1,1},1,1,1},
            "lifecycle rewrote an already captured immutable job");
        expect(!s.pipeline.view(100), "observable cutoff left the old selected/raw view eligible");
        expect(s.pipeline.state(frozenJob.key)->publication == PublicationState::Cancelled,
            "logical transition did not cancel pending publication");
        s.complete(0);
        expect(s.pipeline.state(frozenJob.key)->publication == PublicationState::Cancelled,
            "returned worker/exact completion resurrected old key");
        if (test.next.settings.enabled) {
            s.submit(test.next, 2, 8);
            selected(s.pipeline, 101, 8, OutputKind::PrimingRaw);
        } else s.placeholder(test.next, 2);
    }
    Session s;
    s.submit(observation(), 1, 4); s.complete(0);
    const auto cut = s.observer.prepare(observation(101), true);
    expect(cut.cause == ResetCause::Cut && cut.epochs == Epochs{41,2,2,1} && cut.resetTemporal, "explicit cut not observable");
    s.submit(observation(101), 2, 5);
    selected(s.pipeline, 101, 5, OutputKind::PrimingRaw);
    const auto simultaneous = s.observer.prepare(observation(102,302,902,SourceValidity::Valid,2,true,2560,true));
    expect(simultaneous.epochs == Epochs{41,3,3,2} && s.pipeline.retired_generations() <= 1,
        "simultaneous resource changes allocated multiple replacement generations");
    {
        Session formats;
        formats.submit(observation(),1,7); formats.complete(0);
        formats.submit(observation(101,301,901,SourceValidity::Valid,1,true,1920,true),2,8);
        selected(formats.pipeline,101,8,OutputKind::PrimingRaw);
        formats.complete(1);
        formats.submit(observation(102),3,9);
        selected(formats.pipeline,102,9,OutputKind::PrimingRaw);
        expect(formats.pipeline.epochs() == Epochs{41,3,3,3}, "SDR-HDR-SDR failed to replace exact resource generations");
    }
    std::puts("observable_source/settings/extent/active_rect/format/color/alpha/white/device/disable/cut=PASS");
}
void invalidity_and_reconnect()
{
    for (auto validity : {SourceValidity::Invalid, SourceValidity::Unknown}) {
        Session s;
        s.submit(observation(),1,7);
        await(s.records[0].gate->started);
        const auto invalid = observation(101,301,901,validity);
        const auto decision = s.observer.prepare(invalid);
        expect(decision.epochs == Epochs{41,2,2,1} && decision.resetTemporal &&
            decision.placeholder->reason == (validity == SourceValidity::Invalid ? Reason::InvalidSource : Reason::UnknownValidity),
            "explicit invalidity/unknown signal misclassified");
        const auto repeated = s.observer.prepare(invalid);
        expect(repeated.epochs == decision.epochs && !repeated.resetTemporal, "repeated invalid observation churned epochs");
        s.placeholder(invalid,2);
        s.complete(0);
        s.submit(observation(102),3,8);
        selected(s.pipeline,102,8,OutputKind::PrimingRaw);
    }
    Session s;
    s.submit(observation(),1,7); s.complete(0);
    s.placeholder(observation(101,301,901,SourceValidity::Valid,1,false),2);
    const auto enabled = s.observer.prepare(observation(102));
    expect(enabled.cause == ResetCause::Disabled && enabled.resetTemporal && !enabled.placeholder,
        "reenable did not re-prime both histories");
    s.submit(observation(102),3,8);
    selected(s.pipeline,102,8,OutputKind::PrimingRaw);
    for (uint64_t tick = 103; tick < 107; ++tick) {
        s.complete(s.records.size()-1);
        const auto before = s.pipeline.epochs();
        s.submit(observation(tick),tick-99,8); // Actual identical pixels, different ingress.
        selected(s.pipeline,tick,8,OutputKind::DelayedNr);
        expect(s.pipeline.epochs() == before, "valid identical pixels fabricated HDMI loss");
    }
    for (const auto& invalid : std::array<Observation,4>{
        observation(107,0),observation(107,301,0),
        observation(107,301,901,SourceValidity::Valid,0),
        observation(107,301,901,SourceValidity::Valid,1,true,0)}) {
        Session boundary;
        const auto decision = boundary.observer.prepare(invalid);
        expect(decision.placeholder && !decision.placeholder->frame && !decision.placeholder->job,
            "unsupported boundary admitted temporal output");
    }
    std::puts("invalid/unknown/current_placeholder/reconnect/reenable/identical_valid_holds=PASS");
}
void address_reuse_and_instances()
{
    Session a(41), b(42);
    std::optional<uint64_t> source;
    source.emplace(301);
    const auto* address = &*source;
    const auto old = a.submit(observation(100,*source),1,11);
    await(a.records[0].gate->started);
    b.submit(observation(),1,22); b.complete(0);
    source.reset(); source.emplace(302);
    expect(address == &*source, "source-object address reuse fixture failed");
    const auto changed = a.observer.prepare(observation(101,*source));
    expect(changed.epochs == Epochs{41,2,2,1}, "lifetime token was replaced by pointer identity");
    a.complete(0);
    expect(a.pipeline.state(old.admission->job.key)->publication == PublicationState::Cancelled,
        "old completion published after source address reuse");
    a.submit(observation(101,*source),2,33);
    b.submit(observation(101),2,44);
    selected(a.pipeline,101,33,OutputKind::PrimingRaw);
    selected(b.pipeline,101,22,OutputKind::DelayedNr);
    expect(b.pipeline.epochs() == Epochs{42,1,1,1}, "one instance reset the other's epochs");
    const auto stale = sample({41,1,1,1},observation(102,301),3);
    const auto rejected = a.pipeline.begin(stale,103);
    a.lastTick = 102;
    expect(!rejected.admission && !rejected.selection.frame && rejected.selection.kind == OutputKind::CurrentSource,
        "old immutable ingress key admitted after source reuse");
    expect(a.pipeline.finish(nullptr), "old-key refusal did not latch placeholder");
    a.shutdown(); b.shutdown();
    std::optional<FramePipeline> pipelineStorage;
    std::optional<Observer> observerStorage;
    auto processor = std::make_shared<CpuProcessor>();
    pipelineStorage.emplace(Epochs{51,1,1,1},processor);
    observerStorage.emplace(*pipelineStorage);
    const auto* oldPipeline = &*pipelineStorage;
    const auto* oldObserver = &*observerStorage;
    (void)observerStorage->prepare(observation());
    observerStorage.reset(); close(*pipelineStorage); pipelineStorage.reset();
    pipelineStorage.emplace(Epochs{52,1,1,1},processor);
    observerStorage.emplace(*pipelineStorage);
    expect(oldPipeline == &*pipelineStorage && oldObserver == &*observerStorage, "filter-instance address reuse fixture failed");
    const auto fresh = observerStorage->prepare(observation());
    expect(fresh.epochs == Epochs{52,1,1,1} && !fresh.placeholder, "re-added instance inherited observer state");
    const auto wrong = pipelineStorage->begin(sample({51,1,1,1},observation(),1),101);
    expect(!wrong.admission && !wrong.selection.frame && pipelineStorage->finish(nullptr), "re-added instance admitted old instance ID");
    observerStorage.reset(); close(*pipelineStorage);
    std::puts("source/filter_address_reuse/old_completion/immutable_old_key/two_independent_instances=PASS");
}
void retirement_and_pins()
{
    Session s;
    struct Lease { std::shared_ptr<int> feature, upload; };
    std::array<std::weak_ptr<Lease>,3> bundle;
    std::array<std::weak_ptr<int>,3> feature, upload;
    for (uint64_t g = 1; g <= 3; ++g) {
        auto lease = std::make_shared<Lease>(Lease{std::make_shared<int>(1),std::make_shared<int>(2)});
        bundle[g-1] = lease; feature[g-1] = lease->feature; upload[g-1] = lease->upload;
        s.hold(g,lease);
        if (g < 3) {
            const auto update = s.observer.prepare(observation(99+g,301,901,SourceValidity::Valid,g+1));
            expect(!update.placeholder && update.epochs.resourceGeneration == g+1, "first two generations not retired");
        }
    }
    const auto plan = s.submit(observation(102,301,901,SourceValidity::Valid,3),1,7);
    await(s.records[0].gate->started);
    const auto key = plan.admission->job.key;
    const auto before = s.pipeline.epochs(); // 41/3/3/3
    const auto desired = observation(103,301,901,SourceValidity::Valid,4);
    expect(!s.pipeline.reset(ResetCause::SettingsChanged) && s.pipeline.epochs() == before,
        "accepted resource-budget refusal unexpectedly advanced epochs");
    const auto blocked = s.observer.prepare(desired);
    expect(blocked.placeholder && blocked.placeholder->reason == Reason::RetirementBudget &&
        !blocked.placeholder->frame && !blocked.placeholder->job && blocked.resetTemporal &&
        blocked.epochs == Epochs{41,4,4,3} && before == Epochs{41,3,3,3},
        "failed resource replacement did not cut logically and visibly refuse old-resource submission");
    expect(s.pipeline.retired_generations() == 2 && !s.pipeline.complete_generation(4),
        "budget refusal fabricated generation4 or its completion");
    const auto repeated = s.observer.prepare(desired);
    expect(repeated.placeholder->reason == Reason::RetirementBudget && repeated.epochs == blocked.epochs &&
        !repeated.resetTemporal, "blocked observation churned epochs or lost admission refusal");
    for (size_t i = 0; i < bundle.size(); ++i)
        expect(!bundle[i].expired() && !feature[i].expired() && !upload[i].expired(), "unresolved feature/upload freed");
    expect(s.pipeline.state(key)->publication == PublicationState::Cancelled &&
        !s.pipeline.state(key)->dispatchReturned, "logical cutoff guessed worker completion");
    s.complete(0,1,true);
    s.pipeline.collect();
    expect(s.pipeline.state(key).has_value(), "producer-only receipt freed worker/open-pin slot");
    s.complete(0,3,true);
    s.pipeline.collect();
    expect(s.pipeline.state(key).has_value(), "returned worker freed open capture/consumer pin");
    s.complete(0,3,false);
    s.pipeline.collect();
    expect(s.pipeline.state(key).has_value(), "ended pin without consumer completion reused slot");
    s.complete(0,7,false);
    s.pipeline.collect();
    expect(!s.pipeline.state(key), "exact complete producer/worker/consumer did not collect slot");
    expect(s.pipeline.complete_generation(1), "exact retired feature/upload receipt refused");
    s.pipeline.collect();
    expect(bundle[0].expired() && feature[0].expired() && upload[0].expired() && s.pipeline.retired_generations() == 1,
        "exact retired feature/upload collection did not reclaim budget");
    const auto resumed = s.observer.prepare(desired);
    expect(!resumed.placeholder && resumed.resetTemporal && resumed.epochs == Epochs{41,5,5,4},
        "resource retry did not resume through accepted replacement after real fixture receipt");
    s.submit(desired,2,8);
    selected(s.pipeline,103,8,OutputKind::PrimingRaw);
    s.shutdown();
    for (size_t i = 0; i < bundle.size(); ++i)
        expect(bundle[i].expired() && feature[i].expired() && upload[i].expired(), "owned feature/upload leaked after resolved close");
    std::puts("two_retired_generations/failed_reset_logical_cutoff/exact_feature_upload/producer_worker_open_pin_consumer=PASS");
}
void missed_evaluations_and_slots()
{
    {
        Session s;
        s.submit(observation(),1,7);
        await(s.records[0].gate->started);
        const auto before = s.pipeline.epochs();
        s.submit(observation(101),2,8);
        selected(s.pipeline,101,7,OutputKind::DelayedRaw);
        expect(s.pipeline.epochs() == Epochs{41,1,2,1} && before == Epochs{41,1,1,1},
            "missed evaluation did not jointly reset history while preserving exact raw n-1");
        s.complete(0);
        selected(s.pipeline,101,7,OutputKind::DelayedRaw);
        s.complete(1);
        s.submit(observation(102),3,9);
        selected(s.pipeline,102,8,OutputKind::DelayedNr);
    }
    {
        Session s;
        s.submit(observation(),1,7,std::make_shared<Gate>(false)); s.complete(0);
        const auto failed = s.submit(observation(101),2,8);
        expect(failed.selection.reason == Reason::EvaluationFailure && failed.selection.resetTemporal &&
            s.pipeline.epochs() == Epochs{41,1,2,1}, "failed evaluation lost coupled reset");
        selected(s.pipeline,101,7,OutputKind::DelayedRaw);
    }
    {
        Session s;
        s.submit(observation(),1,7); s.complete(0);
        const auto gap = s.submit(observation(103),2,8);
        expect(gap.selection.kind == OutputKind::CurrentSource && !gap.selection.frame && !gap.admission &&
            !s.pipeline.view(103)->image, "missed canonical predecessor substituted last successful capture");
        s.submit(observation(104),3,9);
        selected(s.pipeline,104,9,OutputKind::PrimingRaw);
    }
    {
        Session s;
        for (uint64_t n = 0; n < 6; ++n) {
            const auto plan = s.submit(observation(100+n),n+1,n);
            if (!n) await(s.records[0].gate->started);
            if (n < 3) expect(plan.admission && plan.admission->slot == n, "unfinished three-slot ring reused a slot");
            else expect(!plan.admission && plan.admissionReason == Reason::SlotsFull,
                "slot saturation waited or allocated slot4");
        }
        expect(s.pipeline.counters().dispatches == 3 && s.pipeline.counters().captures == 6,
            "three-slot bound lost independent raw fallback");
        const auto exited = s.pipeline.worker_exit();
        s.pipeline.request_stop();
        expect(!s.pipeline.try_close(), "unresolved fixture ownership was force-closed");
        for (size_t i = 0; i < s.records.size(); ++i) s.complete(i);
        await(exited);
        s.pipeline.collect();
        expect(s.pipeline.try_close(), "exact resolved fixture stop did not close");
        // This intentional held-stop case does not assert native session order.
        s.records.clear(); s.closed = true; --activeSessions;
    }
    {
        auto processor = std::make_shared<CpuProcessor>();
        FramePipeline p({41,1,UINT64_MAX,1},processor);
        Observer observer(p);
        (void)observer.prepare(observation());
        const auto exhausted = observer.prepare(observation(101,301,902));
        expect(exhausted.placeholder && exhausted.placeholder->reason == Reason::KeyMismatch &&
            !exhausted.resetTemporal && exhausted.epochs.historyEpoch == UINT64_MAX, "exhausted epoch wrapped or claimed reset");
        close(p);
    }
    std::puts("deadline_miss/evaluation_failure/same_phase_late_refusal/three_slots/nonblocking_close/epoch_exhaustion=PASS");
}
void worker_generation_service()
{
    struct Service final : Processor {
        std::promise<void> serviced;
        std::atomic<bool> requested{false};
        Processed process(const Work&, std::stop_token) override
        {
            throw std::runtime_error("generation work reached the service owner");
        }
        void worker_service() noexcept override
        {
            if (requested.exchange(false)) serviced.set_value();
        }
    };
    auto service = std::make_shared<Service>();
    auto worker = std::make_shared<CpuProcessor>();
    FramePipeline p({41,1,1,1},service);
    Observer observer(p);
    const auto o=observation();
    const auto decision=observer.prepare(o);
    const auto input=sample(decision.epochs,o,1);
    const auto plan=p.begin(input,101);
    expect(plan.admission.has_value(),"generation integration admission missing");
    auto gate=std::make_shared<Gate>();
    auto raw=std::make_shared<const RawFrame>(RawFrame{input,std::make_shared<const Image>(Image{7})});
    auto captured=receipt(*plan.admission,0,true);
    captured.phase=interop::WorkPhase::Captured;captured.worker=0;
    const WorkBinding binding{{plan.admission->job.key,plan.admission->slot},captured,{},gate,{},worker};
    expect(p.finish(raw,&binding),"exact generation-bound dispatch refused");
    await(gate->started);
    gate->release();await(plan.admission->returned);
    expect(p.observe(binding.ticket,receipt(*plan.admission)),"generation ownership receipt refused");
    expect(p.cancel(plan.admission->job.key),"generation cancellation refused");
    const auto invalid=observation(101,301,901,SourceValidity::Invalid);
    (void)observer.prepare(invalid);
    (void)p.begin(sample(p.epochs(),invalid,2),102);
    expect(p.finish({}) && !p.view(101)->image,"invalid generation retained raw output");
    p.collect();raw.reset();
    const auto serviced=service->serviced.get_future().share(); // Subscribe before notify.
    service->requested.store(true);
    p.service();
    await(serviced);
    close(p);
    std::puts("production_generation_bound_worker/exact_receipts/worker_only_service/invalid_capture_free_cutoff=PASS");
}
void unsupported_runtime_observation()
{
    Session s;
    s.submit(observation(),1,7);s.complete(0);
    const auto before=s.pipeline.epochs();
    const auto unsupported=s.observer.prepare(observation(101),false,Reason::InvalidSettings);
    expect(unsupported.placeholder && unsupported.placeholder->reason==Reason::InvalidSettings &&
        unsupported.resetTemporal && !s.pipeline.view(100),"unadmitted runtime retained the production view");
    const auto repeated=s.observer.prepare(observation(102),false,Reason::InvalidSettings);
    expect(repeated.epochs==unsupported.epochs && !repeated.resetTemporal &&
        repeated.epochs.contentEpoch==before.contentEpoch+1,"repeated unsupported runtime churned epochs");
    const auto valid=s.observer.prepare(observation(103));
    expect(valid.resetTemporal && !valid.placeholder,"supported runtime reconnect did not re-prime");
    std::puts("unadmitted_runtime/actual_settings_boundary/one_cutoff/reconnect=PASS");
}
void snapshot_retention_reentry()
{
    struct CaptureLease {
        const std::shared_ptr<std::promise<void>> released;
        explicit CaptureLease(std::shared_ptr<std::promise<void>> signal):released(std::move(signal)) {}
        ~CaptureLease() { released->set_value(); }
    };
    auto aSignal=std::make_shared<std::promise<void>>(),bSignal=std::make_shared<std::promise<void>>();
    const auto aReleased=aSignal->get_future().share(),bReleased=bSignal->get_future().share();
    auto a=std::make_shared<const CaptureLease>(aSignal),b=std::make_shared<const CaptureLease>(bSignal);
    std::weak_ptr<const CaptureLease> aLease=a,bLease=b;
    SnapshotRetirement old(11);
    expect(old.capture_allowed() && old.retire({a,b},7),"initial bounded raw-only cutoff refused");
    expect(old.retire({a,b},7),"identical cutoff reentry refused");
    a.reset();b.reset(); // No dispatched job or record owns these independent captures.
    for(unsigned entry=0;entry<3;++entry) {
        expect(old.retire({},7),"null clear_views/try_close reentry refused");
        expect(!old.capture_allowed() && !aLease.expired() && !bLease.expired(),
            "pending graphics captures released by repeated cutoff");
    }
    expect(!old.collect(12,7) && !old.collect(11,6) && !old.collect(11,UINT64_MAX),
        "wrong generation/pending/failed fence released snapshot owners");

    auto cSignal=std::make_shared<std::promise<void>>(),dSignal=std::make_shared<std::promise<void>>();
    const auto cReleased=cSignal->get_future().share(),dReleased=dSignal->get_future().share();
    auto c=std::make_shared<const CaptureLease>(cSignal),d=std::make_shared<const CaptureLease>(dSignal);
    std::weak_ptr<const CaptureLease> cLease=c,dLease=d;
    expect(!old.capture_allowed() && !old.retire({c,d},8),
        "later captures replaced a pending pair or expanded its bounded budget");
    expect(!aLease.expired() && !bLease.expired() && !cLease.expired() && !dLease.expired(),
        "refused replacement dropped old or still caller-owned new captures");
    expect(old.collect(11,7),"exact old snapshot fence did not collect");
    await(aReleased);await(bReleased);
    expect(aLease.expired() && bLease.expired() && old.capture_allowed(),"exact old lease release failed");
    expect(old.retire({c,d},8),"new capture pair refused after exact old completion");
    c.reset();d.reset();
    expect(old.retire({},8) && !old.collect(11,7) && !cLease.expired() && !dLease.expired(),
        "stale completion or reentry released a later pair");

    auto eSignal=std::make_shared<std::promise<void>>();
    const auto eReleased=eSignal->get_future().share();
    auto e=std::make_shared<const CaptureLease>(eSignal);
    std::weak_ptr<const CaptureLease> eLease=e;
    SnapshotRetirement replacement(12);
    expect(replacement.retire({e,e},1),"replacement generation independent cutoff refused");
    e.reset();
    expect(old.retire({},8) && !replacement.collect(11,8) && !old.collect(12,8) &&
        !cLease.expired() && !dLease.expired() && !eLease.expired(),
        "generation ordering crossed exact capture ownership");
    expect(replacement.collect(12,1),"exact replacement fence did not collect");
    await(eReleased);
    expect(eLease.expired() && !cLease.expired() && !dLease.expired(),
        "replacement completion released the retired generation");
    expect(old.collect(11,8),"exact later old-generation fence did not collect");
    await(cReleased);await(dReleased);
    expect(cLease.expired() && dLease.expired() && old.capture_allowed(),
        "later independent captures failed exact release");
    std::puts("snapshot_retention raw_only/reentry/pending_failed_wrong_generation/refused_new_capture/replacement/exact_release=PASS");
}
void closure_diagnostics()
{
    static_assert(std::is_trivially_copyable_v<CloseSnapshot> && std::is_standard_layout_v<CloseSnapshot>);
    static_assert(sizeof(CloseSnapshot)<4096);
    CloseSlot slot{17,4,4,7,7,9,9,true,true,true,false};
    expect(close_slot(slot)==CloseBlock::None,"exact slot receipts refused");
    const std::array<uint64_t CloseSlot::*,3> completed{&CloseSlot::producerDone,&CloseSlot::workerDone,&CloseSlot::consumerDone};
    const std::array<CloseBlock,3> blocked{CloseBlock::Producer,CloseBlock::Worker,CloseBlock::Consumer};
    for(size_t i=0;i<completed.size();++i)for(const auto value:{uint64_t{0},UINT64_MAX}) {
        auto pending=slot;pending.*completed[i]=value;
        expect(close_slot(pending)==blocked[i],"incomplete or unknown physical fence accepted");
    }
    auto unready=slot;unready.ready=false;
    expect(close_slot(unready)==CloseBlock::Unready,"unready job accepted");
    auto unreturned=slot;unreturned.returned=false;
    expect(close_slot(unreturned)==CloseBlock::Unreturned,"unreturned dispatch accepted");
    auto pinned=slot;pinned.pin=true;
    expect(close_slot(pinned)==CloseBlock::OpenPin,"open consumer pin accepted");
    expect(close_entry(5,6,false,0,0)==CloseBlock::WrongOwner,"wrong graphics owner accepted");
    expect(close_entry(5,5,true,19,19)==CloseBlock::MissingTick,"same canonical tick accepted");
    expect(close_entry(5,5,true,20,19)==CloseBlock::None,"later canonical tick refused");

    ModelClose model;
    model.settlement=model.feature=model.ofa=model.module=model.core=CloseResult::Complete;
    model.coreCalled=true;model.coreResult=1;
    expect(close_model(model)==CloseBlock::None,"exact model release receipts refused");
    const std::array<CloseResult ModelClose::*,5> stages{&ModelClose::settlement,&ModelClose::feature,
        &ModelClose::ofa,&ModelClose::module,&ModelClose::core};
    const std::array<CloseBlock,5> stagesBlocked{CloseBlock::ModelSettlement,CloseBlock::FeatureRelease,
        CloseBlock::OfaRelease,CloseBlock::ModuleRelease,CloseBlock::CoreShutdown};
    for(size_t i=0;i<stages.size();++i)for(const auto value:{CloseResult::Unknown,CloseResult::Pending,CloseResult::Refused}) {
        auto refused=model;refused.*stages[i]=value;
        expect(close_model(refused)==stagesBlocked[i],"unproved worker release stage accepted");
    }
    auto bad=model;bad.coreRemaining=1;
    expect(close_model(bad)==CloseBlock::CoreShutdown,"CORE success with remaining owner accepted");
    bad=model;bad.coreResult=0xbad;
    expect(close_model(bad)==CloseBlock::CoreShutdown,"actual CORE failure accepted");
    bad=model;bad.moduleRequired=true;bad.modulePresent=true;
    expect(close_model(bad)==CloseBlock::ModuleRelease,"still-loaded owned module accepted");
    bad.modulePresent=false;
    expect(close_model(bad)==CloseBlock::None,"exact unloaded module remained refused");
    {
        auto retired=model;
        retired.sdk=SdkDisposition::Retired;retired.sessionId=1;retired.ownersAtDetachment=1;
        retired.moduleReferenceReleased=retired.iatRestored=retired.coreReferenceReleased=true;
        retired.vendor.coherent=true;retired.vendor.owner=1;
        auto& sd=retired.vendor.calls[static_cast<size_t>(VendorStage::CoreShutdown)].latest;
        auto& rl=retired.vendor.calls[static_cast<size_t>(VendorStage::CoreLibraryRelease)].latest;
        sd.returned=5;sd.result=1;rl.returned=6;rl.result=1;
        expect(sdk_retirement_complete(retired),"exact SDK retirement receipts refused");
        sd.error=0x534B4950u;
        expect(!sdk_retirement_complete(retired),"skipped Shutdown1 counted as proven retirement");
    }
    bad=model;bad.moduleError=5;
    expect(close_model(bad)==CloseBlock::ModuleRelease,"module release failure accepted");
    bad=model;bad.exception=9;
    expect(close_model(bad)==CloseBlock::Exception,"worker exception accepted");
    bad=model;bad.coherent=false;
    expect(close_model(bad)==CloseBlock::ModelSettlement,"in-progress publication accepted");

    CloseGeneration generation;
    generation.id=11;generation.closed=generation.requested=generation.quiescent=generation.hasModel=true;
    generation.graphics=generation.graphicsDone=7;generation.model=model;
    generation.bridge=generation.generation=CloseResult::Complete;
    expect(close_generation(generation)==CloseBlock::None,"exact generation collection refused");
    generation.cutoffLeases=1;
    expect(close_generation(generation)==CloseBlock::CutoffLease,"outstanding snapshot lease accepted");
    generation.cutoffLeases=0;generation.graphicsDone=UINT64_MAX;
    expect(close_generation(generation)==CloseBlock::Graphics,"unknown graphics fence accepted");
    generation.graphicsDone=6;
    expect(close_generation(generation)==CloseBlock::Graphics,"pending graphics fence accepted");
    generation.graphicsDone=7;generation.bridge=CloseResult::Refused;
    expect(close_generation(generation)==CloseBlock::Bridge,"bridge refusal accepted");
    generation.bridge=CloseResult::Complete;generation.generation=CloseResult::Refused;
    expect(close_generation(generation)==CloseBlock::Generation,"scheduler generation refusal accepted");
    generation.generation=CloseResult::Complete;generation.model.coreRemaining=1;
    expect(close_generation(generation)==CloseBlock::CoreShutdown,"closed worker bypassed actual CORE refusal");
    generation.model=model;
    CloseSnapshot snapshot;
    snapshot.instance=91;snapshot.stop=snapshot.closed=snapshot.workerExited=true;
    snapshot.pipeline=CloseResult::Complete;snapshot.generations[0]=generation;
    expect(close_complete(snapshot),"exact complete primitive close refused");
    snapshot.pipeline=CloseResult::Refused;
    expect(!close_complete(snapshot),"downstream pipeline refusal accepted");
    snapshot.pipeline=CloseResult::Complete;snapshot.closed=false;
    expect(!close_complete(snapshot),"aborted unresolved worker exit became normal closure");
    snapshot.closed=true;snapshot.generations[0].model.coreRemaining=1;
    expect(!close_complete(snapshot),"worker exit overrode CORE remaining ownership");

    expect(!phase_accepted(true,false,true,false,false,true),"supported placeholder emitted accepted row");
    expect(!phase_accepted(true,false,false,true,true,true),"cold reset emitted edited accepted row");
    expect(!phase_accepted(true,false,false,true,false,false),"unapplied settings emitted accepted row");
    expect(phase_accepted(true,false,false,true,false,true),"real edited applied-settings row refused");
    expect(phase_accepted(true,true,true,false,false,true),"declared invalid placeholder refused");
    expect(!phase_accepted(false,false,true,false,false,true),"transport transient placeholder accepted");
    expect(phase_accepted(false,false,false,false,false,true),"supported transport row refused");
    expect(replacement_instance(41,42) && replacement_instance(41,42),"warmup replaced original instance baseline");
    expect(!replacement_instance(41,41) && !replacement_instance(0,42),"reused or absent instance accepted");
    std::puts("closure diagnostics physical_fences/dispatch/pins/owner/tick/model/OFA/CORE/module/downstream/abort/fixture=PASS");
}
void cooperative_stop_retry()
{
    class Refusing final : public Processor {
    public:
        std::promise<void> first, retried;
        std::atomic<unsigned> calls{0};
        std::atomic<bool> allowed{false}, closed{false};
        Processed process(const Work& work,std::stop_token) override { return {work.job,false,{}}; }
        void worker_stopping() noexcept override {
            const auto n=++calls;
            closed.store(allowed.load(std::memory_order_acquire),std::memory_order_release);
            if(n==1)first.set_value();
            if(closed.load() && n>1)retried.set_value();
        }
        bool worker_stop_complete() const noexcept override { return closed.load(std::memory_order_acquire); }
    };
    auto processor=std::make_shared<Refusing>();
    const auto refused=processor->first.get_future().share();
    const auto retry=processor->retried.get_future().share();
    FramePipeline pipeline({93,1,1,1},processor);
    const auto exit=pipeline.worker_exit();
    pipeline.request_stop();
    await(refused);
    processor->allowed.store(true,std::memory_order_release);
    pipeline.service();
    const bool retried=retry.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    await(exit);
    const bool collected=pipeline.try_close();
    expect(retried && collected && processor->calls>=2,
        "stopped production worker stranded retryable cleanup owner");
    std::puts("PASS production FramePipeline stop/refusal/service/retry/exit/collection");
}
int without_observer()
{
    unsigned missing = 0;
    for (unsigned change = 0; change < 5; ++change) {
        FramePipeline pipeline({41, 1, 1, 1}, std::make_shared<CpuProcessor>());
        const IngressSample input{{41, 1, 1}, 100, 99, 301, change == 3 ? 902u : 901u, 1,
            SourceValidity::Valid, descriptor(change == 1 ? 2560u : 1920u, change == 2),
            settings(change == 0 ? 2u : 1u, change != 4)};
        const auto plan = pipeline.begin(input, 101);
        const auto observed = pipeline.epochs();
        const bool reset = change == 4 ? observed.historyEpoch == 2 && observed.contentEpoch == 2 :
            observed.resourceGeneration == 2 && observed.historyEpoch == 2 && observed.contentEpoch == 2;
        close(pipeline);
        std::printf("UNWIRED change=%u generation=%llu content=%llu history=%llu selection=%u reset_observed=%u cleanup=closed\n",
            change, observed.resourceGeneration, observed.contentEpoch, observed.historyEpoch,
            static_cast<unsigned>(plan.selection.kind), reset);
        if (!reset) ++missing;
    }
    std::printf("BASELINE missing_observable_resets=%u expected=0 CPU_only=true\n", missing);
    return missing ? 1 : 0;
}
void identity()
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    wchar_t image[32768]{};
    expect(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != 0, "CPU process creation identity absent");
    expect(GetModuleFileNameW(nullptr, image, 32768) != 0, "CPU executable image absent");
    const uint64_t created = (uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
    std::printf("CPU_PROCESS pid=%lu creation_filetime=%llu image=%ls\n", GetCurrentProcessId(), created, image);
}
}
static void key_diagnostic() {
    RawKeyDiagnostic d;
    d.instance=7;d.tick=10;d.selected=4;d.contentEpoch=3;
    expect(raw_key_failure(d)==7,"missing raw/current/context conjuncts");
    d.rawPresent=d.currentPresent=d.contextPresent=true;
    d.raw={7,3,4};d.current={7,3,5};
    expect(raw_key_failure(d)==0,"actual selected immutable frame");
    d.raw.ingress=5;expect(raw_key_failure(d)==8,"exact ingress conjunct");
    d.raw.ingress=4;d.raw.contentEpoch=2;expect(raw_key_failure(d)==16,"exact epoch conjunct");
    d.raw.ingress=5;expect(raw_key_failure(d)==24,"all false key conjuncts retained");
    d.raw={8,3,4};expect(raw_key_failure(d)==0,"diagnostic does not widen original assertion with invented instance rejection");
    expect(d.raw.instance!=d.instance && d.current.ingress==5,"full keys retain owner mismatch for diagnosis");
    std::printf("PASS raw_key_diagnostic complete original-conjunct mask; owner evidence independent CPU_only=true\n");
}
static void cold_observation() {
    HostObservation o;
    o.key={7,10,4,3,true,true,true,{7,3,4},{7,3,5}};
    o.kind=OutputKind::PrimingRaw;o.validity=SourceValidity::Valid;o.ingress=4;
    o.bridgePresent=o.physicalKnown=true;
    expect(host_observation_path(o)==ObservationPath::NativeProvenance,"initial PrimingRaw provenance");
    o.kind=OutputKind::CurrentSource;o.placeholder=true;o.key.selected=0;
    o.key.rawPresent=o.key.currentPresent=false;
    expect(host_observation_path(o)==ObservationPath::Placeholder,"recorded placeholder");
    o.placeholder=false;o.key.tick=11;
    expect(host_observation_path(o)==ObservationPath::ColdCurrentSource,"recorded CurrentSource cold observation before provenance assertion");
    expect(!phase_accepted(true,false,false,false,true,true),"cold cannot be genuine accepted");
    expect(!observation_accepted(host_observation_path(o),true,false,false,true,false,true),"cold cannot masquerade as genuine");
    expect(!observation_accepted(host_observation_path(o),false,false,false,false,false,true),"cold not accepted in transport lane");
    const HostLatch latch{7,11,0,3,123,OutputKind::CurrentSource};
    auto second=latch,third=latch;
    expect(second==latch && third==latch,"three immutable cold latches");
    second.instance++;expect(!(second==latch),"view instance mismatch");
    second=latch;second.tick++;expect(!(second==latch),"view tick mismatch");
    second=latch;second.selected++;expect(!(second==latch),"view selection mismatch");
    second=latch;second.contentEpoch++;expect(!(second==latch),"view epoch mismatch");
    second=latch;second.imageHash++;expect(!(second==latch),"view pixels mismatch");
    second=latch;second.kind=OutputKind::PrimingRaw;expect(!(second==latch),"view kind mismatch");
    unsigned frames=0;
    for(unsigned i=0;i<15;++i)expect(advance_unaccepted_frame(frames),"bounded cold warmup rejected early");
    expect(!advance_unaccepted_frame(frames) && frames==16,"persistent cold must fail at original sixteen frame bound");
    const auto cold=o;
    o.key.contextPresent=false;expect(host_observation_path(o)==ObservationPath::Reject,"missing cold context");
    o=cold;o.bridgePresent=false;expect(host_observation_path(o)==ObservationPath::Reject,"missing cold Bridge");
    o=cold;o.jobPresent=true;expect(host_observation_path(o)==ObservationPath::Reject,"cold contradicts selected job");
    o=cold;o.key.selected=1;expect(host_observation_path(o)==ObservationPath::Reject,"cold selected nonzero");
    o=cold;o.key.rawPresent=true;expect(host_observation_path(o)==ObservationPath::Reject,"partial cold owner pair");
    o=cold;o.key.currentPresent=true;expect(host_observation_path(o)==ObservationPath::Reject,"opposite partial pair");
    o=cold;o.key.rawPresent=o.key.currentPresent=true;expect(host_observation_path(o)==ObservationPath::Reject,"CurrentSource pair cannot skip provenance");
    o=cold;o.key.instance=0;expect(host_observation_path(o)==ObservationPath::Reject,"missing current source instance");
    o=cold;o.key.tick=0;expect(host_observation_path(o)==ObservationPath::Reject,"missing current source tick");
    o=cold;o.key.contentEpoch=0;expect(host_observation_path(o)==ObservationPath::Reject,"missing current source epoch");
    o=cold;o.ingress=0;expect(host_observation_path(o)==ObservationPath::Reject,"missing ingress");
    o=cold;o.validity=SourceValidity::Invalid;expect(host_observation_path(o)==ObservationPath::Reject,"invalid input never cold valid");
    o=cold;o.validity=SourceValidity::Unknown;expect(host_observation_path(o)==ObservationPath::Reject,"unknown input never cold valid");
    o=cold;o.physicalKnown=false;expect(host_observation_path(o)==ObservationPath::Reject,"unknown physical source");
    o=cold;o.reason=Reason::EvaluationFailure;expect(host_observation_path(o)==ObservationPath::Reject,"failure reason not cold");
    o=cold;o.kind=static_cast<OutputKind>(99);expect(host_observation_path(o)==ObservationPath::Reject,"unknown kind not cold");
    o=cold;o.kind=OutputKind::DelayedNr;o.key.rawPresent=o.key.currentPresent=true;o.key.selected=4;
    o.key.raw.ingress=5;expect(host_observation_path(o)==ObservationPath::Reject,"cold then corrupt genuine fails immediately");
    o.key.raw.ingress=4;expect(host_observation_path(o)==ObservationPath::NativeProvenance,"recovered genuine strict provenance");
    std::printf("PASS host_cold_observation recorded_sequence/bounds/adversarial CPU_only=true checks=%u\n",checks);
}
int main(int argc, char** argv)
{
    try {
        identity();
        if(argc==2 && std::string_view(argv[1])=="--cold-observation") {cold_observation();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--key-diagnostic") {key_diagnostic();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--stop-retry") {
            cooperative_stop_retry();
            return 0;
        }
        if(argc==2 && std::string_view(argv[1])=="--closure") {
            closure_diagnostics();
            std::printf("PASS lifecycle_closure_diagnostics checks=%u CPU_only=true\n",checks);
            return 0;
        }
        if(argc==2 && std::string_view(argv[1])=="--snapshot-retention") {
            snapshot_retention_reentry();
            std::printf("PASS lifecycle_snapshot_retention checks=%u CPU_only=true\n",checks);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--without-observer") return without_observer();
        if (argc != 1) throw std::runtime_error("unknown lifecycle test invocation");
        observation_transitions();
        invalidity_and_reconnect();
        address_reuse_and_instances();
        retirement_and_pins();
        missed_evaluations_and_slots();
        worker_generation_service();
        unsupported_runtime_observation();
        snapshot_retention_reentry();
        key_diagnostic();
        expect(activeSessions == 0, "CPU session remained live");
        std::printf("PASS lifecycle_contract checks=%u all_owned_workers_and_leases_closed=true CPU_only=true\n",checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL lifecycle_contract: %s checks=%u CPU_only=true\n", e.what(), checks);
        return 1;
    }
}
