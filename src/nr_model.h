// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "frame_pipeline.h"
#include "nr_lifecycle.h"
#include "nr_color.h"
#include "nr_core.h"
#include "nr_guides.h"
#include "nr_runtime.h"
#include <atomic>
#include "nr_qa_fault.h"

namespace nr::model {
struct Config {
    runtime::RuntimeIdentity runtime;
    std::filesystem::path driver, data;
    std::vector<std::byte> codec, color, guides;
    bool qaObservations=false; // Explicit bounded technical host only; no inference input.
    bool qaPreparation=false; // Read-only allocation metadata, no image lease.
};
struct GuidePreparation {
    const JobKey key;
    const std::array<D3D12_RESOURCE_DESC,2> descriptors;
    const uint32_t grid;
};
// Graphics-owned immutable copies, never a Bridge raw slot or an OBS borrow.
struct Frame {
    Microsoft::WRL::ComPtr<ID3D12Resource> original, analysis;
    Microsoft::WRL::ComPtr<ID3D12Fence> producerLease;
    const std::shared_ptr<void> graphicsLease;
    const NV_OF_FENCE_POINT producer;
    Frame(ID3D12Resource*,ID3D12Resource*,std::shared_ptr<void>,NV_OF_FENCE_POINT);
};
// The creating module owns the future, its state manager and final destructor.
// Host references are opaque shared leases; no owning STL future crosses CRTs.
class QaSignal {
public:
    virtual ~QaSignal()=default;
    virtual void bind(const std::shared_future<void>&) noexcept=0;
    virtual bool ready() const noexcept=0;
    virtual bool wait_for(uint32_t milliseconds) const=0;
    virtual uintptr_t owner_module() const noexcept=0;
};
std::shared_ptr<QaSignal> make_qa_signal();
uint64_t qa_signal_live_count();
// Owners handed to a foreign module (vtables, destructors, CRT heap and future
// state of this image) may be released after a successful Close. Pin this image
// before exporting one, so no final release can run after it is unmapped.
bool pin_module_for_export() noexcept;
// Prepared diagnostic destinations. None is an inference input or a production
// slot/pin. The graphics owner supplies exactly one bounded envelope per ingress.
struct Observation {
    enum Stage : size_t { Canonical, Encoded, Keep, Foreign, Resolved,
        CurrentAnalysis, ReferenceAnalysis, Motion, Vectors, Cost, Completed, Count };
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,Count> images;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,Count> imageFootprints{};
    std::optional<JobKey> key,reference;
    Microsoft::WRL::ComPtr<ID3D12Fence> completionFence;
    std::shared_ptr<QaSignal> returned;
    HANDLE observed=nullptr;
    uint64_t serial=0,stages=0,completionValue=0;
    uint32_t grid=0,code=0,invalid=0;
    uintptr_t module=0;
    bool created=false,evaluated=false,composed=false,submitted=false,reset=false,referenceValid=false;
    std::atomic<bool> refused{false};
    std::atomic<bool> finished{false};
    std::array<int64_t,8> chronology{};
    std::array<int64_t,5> timing{};
};
bool observation_destination(ID3D12Resource*,ID3D12Resource*,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT&);
bool copy_observation(ID3D12GraphicsCommandList*,Observation&,const JobKey&,
    const std::array<ID3D12Resource*,Observation::Count>&);
class ObservationSink {
public:
    virtual ~ObservationSink()=default;
    virtual Observation* reserve(const IngressSample&,const pipeline::TickPlan&) noexcept=0;
    virtual void capture(Observation*,ID3D11Texture2D* current,ID3D11Texture2D* selectedRaw,
        ID3D11Texture2D* selected) noexcept=0;
    // QA only, graphics owner only. Physical completion and worker return stay
    // real; a prepared exact key may be unavailable at its publication cutoff.
    virtual bool publication_available(const JobKey&) const noexcept { return true; }
    virtual Fault qa_fault(const JobKey&) const noexcept { return Fault::None; }
};
inline bool observe_publication(pipeline::FramePipeline& pipeline,const interop::Ticket& ticket,
    const interop::Ownership& physical,const ObservationSink* sink=nullptr,bool stopping=false)
{
    const auto state=pipeline.state(ticket.key);
    const auto epochs=pipeline.epochs();
    if(sink && !stopping && state && state->ticket==ticket && state->dispatchReturned &&
        ticket.key.frame.instanceId==epochs.instanceId && ticket.key.frame.contentEpoch==epochs.contentEpoch &&
        ticket.key.historyEpoch==epochs.historyEpoch && ticket.key.resourceGeneration==epochs.resourceGeneration &&
        (state->publication==PublicationState::Pending || state->publication==PublicationState::Completed) &&
        interop::publishable(physical,ticket.key) && !sink->publication_available(ticket.key))
        return true; // Withhold only graphics completion availability, not a physical receipt's fields.
    return pipeline.observe(ticket,physical); // Terminal/retired/stopping owners always collect normally.
}
struct Job {
    struct Resources;
    const std::shared_ptr<const Frame> frame;
    const JobContract contract;
    const ColorExposure exposure;
    const Fault fault;
    OriginalMarker originalMarker;
    const HANDLE observed, gpu;
    std::atomic<bool> ready{false};
    std::atomic<bool> callbackArrived{false},completionClaimed{false};
    std::atomic<HRESULT> originalSignalResult{E_PENDING}; // Actual call result only; omission stays pending.
    std::atomic<bool> published{false};
    std::atomic<uint32_t> qaFailure{0}; // Exact-job diagnostic, never completion.
    bool created=false, evaluated=false, composed=false, submitted=false, eligible=false,referenceValid=false;
    uint32_t code=0, invalid=0;
    uint64_t epoch=0;
    uint64_t creationEpoch=0;
    uint32_t driverCreateCode=0;
    bool driverHandle=false;
    uintptr_t runtimeModule=0;
    std::wstring modulePath;
    std::string diagnostic;
    std::array<int64_t,8> chronology{}; // create/encode/evaluate/compose/execute/submitted/complete/status
    std::array<int64_t,5> qaTiming{}; // process/producer-ready/module-start/feature-start/feature-return; ready publishes.
    std::optional<FlowResult> flow;
    std::optional<JobKey> guideReferenceKey; // Actual SDK reference, absent on cold/reset.
    uint32_t guideGrid=0;
    ID3D12Resource* output=nullptr; // Borrowed from this job's retained Bridge.
    NV_OF_FENCE_POINT completion() const;
    std::array<ID3D12Resource*,5> color_resources() const;
    // Copies made before guide reuse; never retain SDK-registered reference inputs.
    std::array<ID3D12Resource*,4> guide_observation_resources() const;
    std::array<uintptr_t,3> descriptor_heaps() const;
    ID3D12Resource* invalid_status_resource() const;
    // Published by ready's release store, never read while worker writes.
    std::shared_ptr<Resources> resources;
    Observation* observation=nullptr; // Sink outlives worker; detached before envelope reuse/stop.
    // Diagnostic values only; the existing ready release publishes this job-owned snapshot.
    struct ResetDiagnostic {
        bool captured=false,processorReset=false;
        std::optional<JobKey> previousKey;
        const char* setterCause="not-captured";
    };
    std::optional<ResetDiagnostic> resetDiagnostic;
    explicit Job(std::shared_ptr<const Frame>,const JobContract&,ColorExposure,Fault=Fault::None);
    ~Job();
    Job(const Job&)=delete;
    Job& operator=(const Job&)=delete;
};
bool entry_interposed(HMODULE owner,const void* entry);
class Processor final : public pipeline::Processor {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Processor(std::shared_ptr<interop::Context>,Config,uint64_t instance);
    ~Processor() override;
    pipeline::Processed process(const pipeline::Work&,std::stop_token) override;
    void worker_service() noexcept override;
    bool request_original_marker(const std::shared_ptr<Job>&,const JobKey&);
    void worker_stopping() noexcept override;
    bool worker_stop_complete() const noexcept override;
    bool closed() const;
    lifecycle::ModelClose close_diagnostics() const;
    uint64_t applied() const;
    std::shared_ptr<const GuidePreparation> guide_preparation() const;
    bool publish(const std::shared_ptr<Job>&,const Selection&,const interop::Ownership&);
};
} // namespace nr::model
