// SPDX-License-Identifier: GPL-3.0-only
#include "nr_instance_scenario.h"
#include <stdexcept>
namespace nr::qa {
namespace {
void require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
void prepared(const InstanceTick& tick) {
    require(tick.source && tick.target && tick.source!=tick.target,"instance source/target ownership");
    require(tick.observation && tick.observation->observed && tick.observation->returned &&
        !tick.observation->finished.load(std::memory_order_acquire),"pre-action observation subscription");
}
void wait(const InstanceTick& tick,obs_adapter::NativeOutput& output) {
    require(output.observation()==tick.observation,"native sink selected another instance envelope");
    require(tick.observation->returned->wait_for(30000),"native exact dispatch return timeout");
    require(WaitForSingleObject(tick.observation->observed,30000)==WAIT_OBJECT_0,
        "native exact physical observation timeout");
    require(tick.observation->finished.load(std::memory_order_acquire) &&
        !tick.observation->refused.load(std::memory_order_acquire),"native observation unfinished/refused");
    const auto job=output.current_job();
    require(job && job->ready.load(std::memory_order_acquire) && job->contract.input==tick.input &&
        job->created && job->evaluated && job->composed && job->submitted &&
        tick.observation->key==job->contract.key,"actual model dispatch/physical identity not proven");
    const auto completion=job->completion();
    require(completion.fence && completion.value &&
        interop::completion(completion.value,completion.fence->GetCompletedValue())==interop::Completion::Complete,
        "native exact model completion fence incomplete");
}
void selected(obs_adapter::NativeOutput& output,const InstanceTick& current,
    const InstanceTick* previous,const Selection& selection) {
    const auto raw=output.selected_raw();
    const auto job=output.selected_job();
    require(raw && raw->input==(previous?previous->input:current.input),"native selected foreign raw snapshot");
    if(previous)require(selection.kind==OutputKind::DelayedNr && selection.job && job &&
        job->contract.input==previous->input && *selection.job==job->contract.key &&
        job->published.load(std::memory_order_acquire),"native selected wrong predecessor/model owner");
    else require(selection.kind==OutputKind::PrimingRaw && !job,"native cold selection not raw");
    for(unsigned view=0;view<4;++view)
        require(output.selected_raw()==raw && output.selected_job()==job,
            "repeated native reference changed selection");
}
}
InstanceObservations::InstanceObservations(ID3D12Device* device,const model::Job& ordinary,
    std::array<std::shared_ptr<model::QaSignal>,3> signals):device_(device),signals_(std::move(signals)) {
    require(device && ordinary.ready.load(std::memory_order_acquire) && ordinary.evaluated &&
        ordinary.flow && !ordinary.flow->reset,"ordinary actual job required for observation descriptors");
    const auto colors=ordinary.color_resources();
    const auto guides=ordinary.guide_observation_resources();
    const std::array<ID3D12Resource*,model::Observation::Count> resources{
        colors[0],colors[1],colors[2],colors[3],colors[4],ordinary.frame->analysis.Get(),
        guides[0],guides[1],guides[2],guides[3],ordinary.output};
    for(size_t i=0;i<resources.size();++i) {
        require(resources[i]!=nullptr,"actual observation descriptor missing");
        descriptors_[i]=resources[i]->GetDesc();
    }
    for(const auto& signal:signals_)require(signal && signal->owner_module(),"original module-owned dispatch subscription");
    for(auto& observation:observations_) {
        observation.observed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        observation.returned=signals_[0]; // Exact reserved slot replaces this before finish/dispatch.
        require(observation.observed && observation.returned,"pre-action native observation event/signal");
    }
}
InstanceObservations::~InstanceObservations() {
    for(auto& observation:observations_)if(observation.observed)CloseHandle(observation.observed);
}
void InstanceObservations::arm(size_t index) {
    require(index<observations_.size() && armed_==observations_.size(),"one prepared envelope per trigger");
    auto& observation=observations_[index];
    require(!observation.key,"observation envelope cannot be replayed");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    for(size_t i=0;i<descriptors_.size();++i) {
        auto descriptor=descriptors_[i];descriptor.Flags=D3D12_RESOURCE_FLAG_NONE;
        require(SUCCEEDED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&descriptor,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&observation.images[i]))),
            "actual prepared observation allocation");
    }
    armed_=index;
}
model::Observation& InstanceObservations::at(size_t index) {
    require(index<observations_.size(),"observation index");return observations_[index];
}
void InstanceObservations::withhold_publication(size_t index) {
    require(index<observations_.size() && armed_==index && !withheldKey_ &&
        withheld_==observations_.size(),"pre-dispatch exact publication availability subscription");
    withheld_=index;
}
void InstanceObservations::release_publication() {
    require(withheldKey_.has_value(),"exact withheld publication key required");
    withheldKey_.reset();withheld_=observations_.size();
}
bool InstanceObservations::publication_available(const JobKey& key) const noexcept {
    return !withheldKey_ || key!=*withheldKey_;
}
model::Observation* InstanceObservations::reserve(const IngressSample& input,const pipeline::TickPlan& plan) noexcept {
    if(armed_>=observations_.size() || !plan.admission || plan.admission->slot>=signals_.size() ||
        plan.admission->job.input!=input)return nullptr;
    const auto index=armed_;
    auto& observation=observations_[index];armed_=observations_.size();
    observation.returned=signals_[plan.admission->slot];
    if(index==withheld_)withheldKey_.emplace(plan.admission->job.key);
    observation.key.emplace(plan.admission->job.key);return &observation;
}
bool run_two_instances(obs_adapter::NativeOutput& a,obs_adapter::NativeOutput& b,
    std::span<const InstanceTickPair> ticks,std::ostream& evidence) {
    try {
        require(&a!=&b && ticks.size()>=3 && ticks.size()<=16,"bounded two-instance native body");
        require(a.context() && b.context() && a.context()!=b.context() && a.transport()!=b.transport(),
            "distinct prepared instance contexts/transports required");
        for(size_t index=0;index<ticks.size();++index) {
            const auto& pair=ticks[index];prepared(pair.a);prepared(pair.b);
            require(pair.a.input.key.instanceId!=pair.b.input.key.instanceId &&
                pair.a.input.settings!=pair.b.input.settings &&
                pair.a.input.sourceIdentity!=pair.b.input.sourceIdentity &&
                pair.a.input.canonicalTick==pair.b.input.canonicalTick,
                "disjoint instance content/settings/source identities on same tick required");
            require(pair.a.source!=pair.b.source && pair.a.target!=pair.b.target &&
                pair.a.source!=pair.b.target && pair.b.source!=pair.a.target &&
                pair.a.observation!=pair.b.observation,"native cross-instance allocation alias");
            if(index) {
                const auto& prior=ticks[index-1];
                require(pair.a.input.precedingCanonicalTick==prior.a.input.canonicalTick &&
                    pair.b.input.precedingCanonicalTick==prior.b.input.canonicalTick,
                    "explicit canonical successor required");
            }
            // Prepared envelopes are subscribed before either action. Both dispatches
            // occur before either wait; A cannot serialize B through host waiting.
            const auto first=index%2?
                b.tick(pair.b.input,pair.b.source,pair.b.target):
                a.tick(pair.a.input,pair.a.source,pair.a.target);
            const auto second=index%2?
                a.tick(pair.a.input,pair.a.source,pair.a.target):
                b.tick(pair.b.input,pair.b.source,pair.b.target);
            wait(pair.a,a);wait(pair.b,b);
            selected(a,pair.a,index?&ticks[index-1].a:nullptr,index%2?second:first);
            selected(b,pair.b,index?&ticks[index-1].b:nullptr,index%2?first:second);
            const auto ja=a.current_job(),jb=b.current_job();
            require(ja!=jb && ja->frame!=jb->frame && ja->frame->original.Get()!=jb->frame->original.Get() &&
                ja->frame->analysis.Get()!=jb->frame->analysis.Get() && ja->output!=jb->output,
                "actual native current model/frame/output allocation alias");
            evidence<<"NR_INSTANCE_BODY {\"tick\":"<<pair.a.input.canonicalTick
                <<",\"instance_a\":"<<pair.a.input.key.instanceId<<",\"instance_b\":"<<pair.b.input.key.instanceId
                <<",\"serial_a\":"<<ja->contract.key.submissionSerial<<",\"serial_b\":"<<jb->contract.key.submissionSerial
                <<",\"actual_evaluated_a\":true,\"actual_evaluated_b\":true,\"selected_owners_disjoint\":true}\n";
        }
        evidence<<"NR_INSTANCE_BODY_COMPLETE scope=ownership_and_selection_only fixture_pixels=caller_owned cleanup=caller_owned\n";
        return true;
    } catch(const std::exception& error) {
        evidence<<"NR_INSTANCE_BODY_FAIL "<<error.what()<<"\n";
        // Caller retains outputs, observations and fixtures until exact closure.
        return false;
    }
}
CapacityObservations::CapacityObservations(std::array<std::shared_ptr<model::QaSignal>,3> signals)
    :signals_(std::move(signals)) {
    for(const auto& signal:signals_)require(signal && signal->owner_module(),"original module capacity signal");
    for(auto& observation:observations_) {
        observation.observed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        observation.returned=signals_[0];
        require(observation.observed!=nullptr,"pre-trigger capacity completion event");
    }
}
CapacityObservations::~CapacityObservations() {
    for(auto& observation:observations_)CloseHandle(observation.observed);
}
size_t CapacityObservations::arm() {
    require(armed_==observations_.size() && next_<observations_.size(),"bounded fresh capacity envelope");
    armed_=next_++;return armed_;
}
model::Observation& CapacityObservations::at(size_t index) {
    require(index<observations_.size(),"capacity envelope index");return observations_[index];
}
model::Observation* CapacityObservations::reserve(const IngressSample& input,const pipeline::TickPlan& plan) noexcept {
    if(armed_>=observations_.size() || (plan.admission &&
        (plan.admission->slot>=signals_.size() || plan.admission->job.input!=input)))return nullptr;
    auto& observation=observations_[armed_];armed_=observations_.size();
    if(plan.admission) {
        observation.key.emplace(plan.admission->job.key);
        observation.returned=signals_[plan.admission->slot];
    }
    return &observation;
}
}
