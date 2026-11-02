// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_model.h"
#include "nr_lifecycle.h"
#include <graphics/graphics.h>
namespace nr::obs_adapter {
inline Selection adapter_selection(const pipeline::TickPlan& plan)
{
    if(plan.admissionReason==Reason::SlotsFull)
        return {plan.selection.canonicalTick,OutputKind::CurrentSource,Status::Bypassed,Reason::SlotsFull,{},{},false};
    return plan.selection;
}
struct TickWitness {
    uint64_t tick;
    Reason admissionReason;
    std::optional<JobKey> admitted;
    pipeline::Counters counters;
};
struct Capture {
    const IngressSample input;
    explicit Capture(const IngressSample& sample):input(sample) {}
    Microsoft::WRL::ComPtr<ID3D11Texture2D> original11, analysis11;
    Microsoft::WRL::ComPtr<ID3D12Resource> original12, analysis12;
};
struct JobWitness {
    const JobKey key;
    const interop::Ownership physical;
    bool ready=false,dispatchReturned=false,flow=false,reset=false;
    bool evaluated=false,composed=false,submitted=false;
    uint32_t publication=0,code=0;
    std::array<int64_t,8> chronology{};
    std::array<char,512> failure{};
    uint32_t failureBytes=0;
    bool failureTruncated=false;
};
struct Diagnostics {
    std::optional<JobWitness> latest,firstCompleted,firstOrdinary;
    std::shared_ptr<const model::GuidePreparation> preparation;
};
class NativeOutput final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeOutput(uint64_t,model::Config,std::vector<std::byte>,std::vector<std::byte>,std::vector<std::byte>,bool deterministicQa=false);
    ~NativeOutput();
    NativeOutput(const NativeOutput&)=delete;
    NativeOutput& operator=(const NativeOutput&)=delete;
    lifecycle::Decision prepare(const lifecycle::Observation&,ID3D11Device*,bool cut=false);
    Selection tick(const IngressSample&,ID3D11Texture2D*,ID3D11Texture2D*);
    void request_stop();
    uint32_t latch_qa_failure(); // Graphics-owned; coupled history cut once.
    bool release_original_marker(const JobKey&); // Exact graphics request; worker signals.
    bool on_graphics_owner() const; // True only on the bound graphics thread; false while the pipeline is unbound (video_tick binds it).
    bool try_close();
    bool closed() const;
    std::shared_future<void> worker_exit() const;
    std::shared_ptr<model::QaSignal> worker_exit_signal();
    std::array<std::shared_ptr<model::QaSignal>,3> dispatch_signals();
    std::shared_ptr<model::Job> current_job() const;
    std::optional<TickWitness> tick_witness() const;
    pipeline::Counters counters() const;
    std::shared_ptr<model::Job> selected_job() const;
    std::shared_ptr<const Capture> selected_raw() const;
    std::shared_ptr<const Capture> current_raw() const;
    std::shared_ptr<interop::Context> context() const;
    std::shared_ptr<interop::Bridge> transport() const;
    Epochs epochs() const;
    size_t retired_generations() const;
    void observe_with(std::shared_ptr<model::ObservationSink>);
    model::Observation* observation() const;
    Diagnostics diagnostics() const;
    lifecycle::CloseSnapshot close_diagnostics(bool sourceDetached=false) const;
};
}
