// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "frame_pipeline.h"
#include <atomic>

namespace nr::lifecycle {
enum class VendorStage : size_t { CoreInit, NrInit, NrShutdown, CapabilityDestroy,
    NrLibraryRelease, CoreShutdown, CoreLibraryRelease, Count };
enum class VendorReturnKind : uint32_t { SdkResult, Win32Bool };
inline VendorReturnKind vendor_return_kind(VendorStage stage) {
    return stage==VendorStage::NrLibraryRelease || stage==VendorStage::CoreLibraryRelease?
        VendorReturnKind::Win32Bool:VendorReturnKind::SdkResult;
}
struct VendorResult { uint32_t result=0,remaining=0,error=0; };
struct VendorBinding { uint64_t module=0,procedure=0,thread=0; };
struct VendorCall {
    uint64_t enter=0,returned=0,result=0,remaining=0,error=0,module=0,procedure=0,thread=0;
};
struct VendorCalls { uint64_t count=0;VendorCall first{},latest{}; };
struct VendorSnapshot {
    uint64_t owner=0,instance=0,generation=0,device=0,sequence=0;
    bool coherent=true,nestedCoreObserved=false; // Existing path-name IAT shims do not observe nested CORE calls.
    std::array<VendorCalls,static_cast<size_t>(VendorStage::Count)> calls{};
};
// One serialized vendor writer at a time. Readers make one bounded atomic copy, never wait.
// Storage is embedded before the owner's resources and contains no resource lease.
class VendorTrace {
    std::atomic<uint64_t> version_{0},sequence_{0};
    std::array<std::atomic<uint64_t>,4> owner_{};
    std::array<std::array<std::atomic<uint64_t>,17>,static_cast<size_t>(VendorStage::Count)> calls_{};
public:
    bool bind(uint64_t owner,uint64_t instance,uint64_t generation,uint64_t device) {
        const std::array values{owner,instance,generation,device};
        if(owner_[0].load()) {
            for(size_t i=0;i<values.size();++i)if(owner_[i].load()!=values[i])return false;
            return true;
        }
        version_.fetch_add(1);
        for(size_t i=0;i<values.size();++i)owner_[i].store(values[i]);
        version_.fetch_add(1);return true;
    }
    template<class Invoke> VendorResult call(VendorStage stage,VendorBinding binding,Invoke&& invoke) {
        auto& c=calls_[static_cast<size_t>(stage)];
        version_.fetch_add(1);
        const auto count=c[0].fetch_add(1)+1;
        const auto enter=sequence_.fetch_add(1)+1;
        const std::array<uint64_t,8> pending{enter,0,0,0,0,binding.module,binding.procedure,binding.thread};
        for(size_t i=0;i<pending.size();++i) {
            c[9+i].store(pending[i]);if(count==1)c[1+i].store(pending[i]);
        }
        version_.fetch_add(1);
        const VendorResult result=invoke(); // Exceptions preserve entered-without-return, not a fabricated result.
        version_.fetch_add(1);
        const std::array<uint64_t,4> returned{sequence_.fetch_add(1)+1,result.result,result.remaining,result.error};
        for(size_t i=0;i<returned.size();++i) {
            c[10+i].store(returned[i]);if(count==1)c[2+i].store(returned[i]);
        }
        version_.fetch_add(1);return result;
    }
    VendorSnapshot snapshot() const {
        VendorSnapshot out;const auto before=version_.load();
        out.owner=owner_[0].load();out.instance=owner_[1].load();
        out.generation=owner_[2].load();out.device=owner_[3].load();out.sequence=sequence_.load();
        for(size_t i=0;i<calls_.size();++i) {
            const auto& c=calls_[i];auto& d=out.calls[i];d.count=c[0].load();
            d.first={c[1].load(),c[2].load(),c[3].load(),c[4].load(),c[5].load(),c[6].load(),c[7].load(),c[8].load()};
            d.latest={c[9].load(),c[10].load(),c[11].load(),c[12].load(),c[13].load(),c[14].load(),c[15].load(),c[16].load()};
        }
        const auto after=version_.load();out.coherent=before==after && !(after&1);return out;
    }
};
struct PrimitiveFrameKey { uint64_t instance=0,contentEpoch=0,ingress=0; };
struct RawKeyDiagnostic {
    uint64_t instance=0,tick=0,selected=0,contentEpoch=0;
    bool rawPresent=false,currentPresent=false,contextPresent=false;
    PrimitiveFrameKey raw{},current{};
};
inline uint32_t raw_key_failure(const RawKeyDiagnostic& d) {
    uint32_t mask=0;
    if(!d.rawPresent)mask|=1;
    if(!d.currentPresent)mask|=2;
    if(!d.contextPresent)mask|=4;
    if(d.rawPresent && d.raw.ingress!=d.selected)mask|=8;
    if(d.rawPresent && d.raw.contentEpoch!=d.contentEpoch)mask|=16;
    return mask;
}
enum class ObservationPath { Reject, Placeholder, ColdCurrentSource, NativeProvenance };
struct HostObservation {
    RawKeyDiagnostic key;
    OutputKind kind=OutputKind::CurrentSource;
    SourceValidity validity=SourceValidity::Unknown;
    Reason reason=Reason::None;
    uint64_t ingress=0;
    bool placeholder=false,bridgePresent=false,jobPresent=false,physicalKnown=false;
};
inline ObservationPath host_observation_path(const HostObservation& o) {
    switch(o.kind) {
    case OutputKind::CurrentSource:
        if(o.placeholder && !o.key.selected)return ObservationPath::Placeholder;
        if(!o.placeholder && !o.key.selected && !o.key.rawPresent && !o.key.currentPresent &&
            o.key.contextPresent && o.bridgePresent && !o.jobPresent && o.physicalKnown &&
            o.key.instance && o.key.tick && o.key.contentEpoch && o.ingress &&
            o.validity==SourceValidity::Valid && o.reason==Reason::None)return ObservationPath::ColdCurrentSource;
        return ObservationPath::Reject;
    case OutputKind::PrimingRaw:
    case OutputKind::DelayedRaw:
    case OutputKind::DelayedNr:
        return !o.placeholder && !raw_key_failure(o.key)?ObservationPath::NativeProvenance:ObservationPath::Reject;
    }
    return ObservationPath::Reject;
}
inline bool advance_unaccepted_frame(unsigned& frames) {return ++frames<16;}
struct HostLatch {
    uint64_t instance=0,tick=0,selected=0,contentEpoch=0,imageHash=0;
    OutputKind kind=OutputKind::CurrentSource;
    bool operator==(const HostLatch&) const=default;
};
enum class CloseBlock : uint32_t { None, WrongOwner, MissingTick, Unready, Unreturned,
    OpenPin, Producer, Worker, Consumer, CutoffLease, Graphics, ModelSettlement,
    FeatureRelease, OfaRelease, ModuleRelease, CoreShutdown, Bridge, Generation,
    Pipeline, Exception, NotStopping };
enum class CloseResult : uint32_t { Unknown, Pending, Complete, Refused, Exception };
enum class SdkDisposition : uint32_t { None, Owned, DetachedShared, Retiring, Retired };
struct CloseSlot {
    uint64_t serial=0,producer=0,producerDone=0,worker=0,workerDone=0,consumer=0,consumerDone=0;
    bool occupied=false,ready=false,returned=false,pin=false;
};
struct ModelClose {
    VendorSnapshot vendor{};
    CloseResult settlement=CloseResult::Unknown,feature=CloseResult::Unknown,ofa=CloseResult::Unknown;
    CloseResult module=CloseResult::Unknown,core=CloseResult::Unknown;
    uint32_t coreResult=0,coreRemaining=0,moduleError=0,exception=0;
    SdkDisposition sdk=SdkDisposition::None; // Uses existing pre-attempt alignment space.
    uint64_t attempts=0;
    bool coreCalled=false,coherent=true;
    uint64_t pendingSerial=0,completionRequired=0,completionDone=0;
    bool callbackRetained=false,guidePending=false;
    bool moduleRequired=false,modulePresent=false;
    // Reference release is not image unmapping. modulePresent describes the
    // session's owned Module reference at this source's detachment.
    bool moduleReferenceReleased=false,iatRestored=false,coreReferenceReleased=false;
    uint64_t sessionId=0,ownersAtDetachment=0;
    uint64_t owners_remaining() const {
        if(sdk==SdkDisposition::DetachedShared && ownersAtDetachment)return ownersAtDetachment-1;
        return sdk==SdkDisposition::Retiring?1u:0u;
    }
};
struct CloseGeneration {
    uint64_t id=0,graphics=0,graphicsDone=0;
    uint32_t cutoffLeases=0;
    bool requested=false,closed=false,quiescent=false,hasModel=false;
    CloseResult bridge=CloseResult::Unknown,generation=CloseResult::Unknown;
    ModelClose model{};
    std::array<CloseSlot,3> slots{};
};
struct CloseSnapshot {
    uint64_t instance=0,sequence=0,tick=0,lastTick=0,retiringCount=0;
    uint32_t owner=0,caller=0;
    uint64_t refusingGeneration=0,refusingSerial=0;
    uint32_t refusingSlot=UINT32_MAX;
    CloseBlock block=CloseBlock::None;
    bool stop=false,workerExited=false,sourceDetached=false,closed=false;
    CloseResult pipeline=CloseResult::Unknown;
    std::array<CloseGeneration,3> generations{};
};
// These predicates are used by the real native close path and its CPU controls.
// Unknown fence values and unreturned dispatch never become completion.
inline CloseBlock close_slot(const CloseSlot& s)
{
    if(!s.occupied)return CloseBlock::None;
    if(!s.ready)return CloseBlock::Unready;
    if(!s.returned)return CloseBlock::Unreturned;
    if(s.pin)return CloseBlock::OpenPin;
    if(s.producerDone==UINT64_MAX || s.producerDone<s.producer)return CloseBlock::Producer;
    if(s.workerDone==UINT64_MAX || s.workerDone<s.worker)return CloseBlock::Worker;
    if(s.consumerDone==UINT64_MAX || s.consumerDone<s.consumer)return CloseBlock::Consumer;
    return CloseBlock::None;
}
inline CloseBlock close_entry(uint32_t owner,uint32_t caller,bool hasInput,uint64_t tick,uint64_t lastTick)
{
    if(owner && owner!=caller)return CloseBlock::WrongOwner;
    if(hasInput && tick<=lastTick)return CloseBlock::MissingTick;
    return CloseBlock::None;
}
inline CloseBlock close_model(const ModelClose& m)
{
    if(!m.coherent)return CloseBlock::ModelSettlement;
    if(m.exception)return CloseBlock::Exception;
    if(m.settlement!=CloseResult::Complete)return CloseBlock::ModelSettlement;
    if(m.feature!=CloseResult::Complete)return CloseBlock::FeatureRelease;
    if(m.ofa!=CloseResult::Complete)return CloseBlock::OfaRelease;
    if(m.sdk==SdkDisposition::DetachedShared) {
        if(!m.sessionId || m.ownersAtDetachment<2)
            return CloseBlock::CoreShutdown;
        if(m.module!=CloseResult::Unknown || m.core!=CloseResult::Unknown ||
            m.coreCalled || m.moduleError || m.moduleReferenceReleased || m.iatRestored || m.coreReferenceReleased)
            return CloseBlock::ModuleRelease;
        return CloseBlock::None;
    }
    if(m.module!=CloseResult::Complete || m.moduleError || (m.moduleRequired && m.modulePresent))return CloseBlock::ModuleRelease;
    if(m.core!=CloseResult::Complete || m.coreRemaining || (m.coreCalled && m.coreResult!=1))return CloseBlock::CoreShutdown;
    if((m.sessionId || m.sdk!=SdkDisposition::None) && (!m.sessionId || m.sdk!=SdkDisposition::Retired || m.ownersAtDetachment!=1 ||
        !m.moduleReferenceReleased || !m.iatRestored || !m.coreReferenceReleased || !m.coreCalled || m.coreResult!=1))
        return CloseBlock::CoreShutdown;
    return CloseBlock::None;
}
inline bool sdk_retirement_complete(const ModelClose& m)
{
    if(close_model(m)!=CloseBlock::None || m.sdk!=SdkDisposition::Retired || !m.sessionId)return false;
    const auto& shutdown=m.vendor.calls[static_cast<size_t>(VendorStage::CoreShutdown)].latest;
    const auto& release=m.vendor.calls[static_cast<size_t>(VendorStage::CoreLibraryRelease)].latest;
    return m.vendor.coherent && m.vendor.owner==m.sessionId && shutdown.returned && shutdown.result==1 && !shutdown.error &&
        !shutdown.remaining && release.returned>shutdown.returned && release.result==1 && !release.error;
}
inline CloseBlock close_generation(const CloseGeneration& g)
{
    for(const auto& slot:g.slots)if(const auto b=close_slot(slot);b!=CloseBlock::None)return b;
    if(g.graphicsDone==UINT64_MAX || g.graphicsDone<g.graphics)return CloseBlock::Graphics;
    if(g.cutoffLeases)return CloseBlock::CutoffLease;
    if(g.hasModel)if(const auto block=close_model(g.model);block!=CloseBlock::None)return block;
    if(!g.closed)return CloseBlock::ModelSettlement;
    if(g.bridge!=CloseResult::Complete)return CloseBlock::Bridge;
    if(g.generation!=CloseResult::Complete)return CloseBlock::Generation;
    return CloseBlock::None;
}
inline bool close_complete(const CloseSnapshot& s)
{
    if(!s.stop || !s.closed || s.retiringCount || s.block!=CloseBlock::None || s.pipeline!=CloseResult::Complete)return false;
    for(const auto& g:s.generations)if(g.id && close_generation(g)!=CloseBlock::None)return false;
    return true;
}
template<class Actions> bool close_model_steps(ModelClose& proof,Actions& actions)
{
    std::array<CloseResult*,5> stages{&proof.settlement,&proof.feature,&proof.ofa,&proof.module,&proof.core};
    for(size_t i=0;i<stages.size();++i) {
        *stages[i]=CloseResult::Pending;
        if(!actions.step(i)){*stages[i]=CloseResult::Refused;return false;}
        if(i==3 && proof.sdk==SdkDisposition::DetachedShared) {
            proof.module=proof.core=CloseResult::Unknown;
            return close_model(proof)==CloseBlock::None;
        }
        *stages[i]=CloseResult::Complete;
    }
    return close_model(proof)==CloseBlock::None;
}
template<class Observe> CloseBlock close_entry_steps(CloseSnapshot& proof,bool hasInput,
    uint64_t tick,uint64_t lastTick,Observe&& observe)
{
    if(!proof.stop)return CloseBlock::NotStopping;
    if(const auto block=close_entry(proof.owner,proof.caller,hasInput,tick,lastTick);block!=CloseBlock::None)return block;
    observe();
    return CloseBlock::None;
}
template<class Actions> CloseBlock close_generation_steps(CloseGeneration& proof,bool jobs,Actions& actions)
{
    for(const auto& slot:proof.slots)if(const auto block=close_slot(slot);block!=CloseBlock::None)return block;
    if(proof.graphicsDone==UINT64_MAX || proof.graphicsDone<proof.graphics)return CloseBlock::Graphics;
    if(proof.cutoffLeases)return CloseBlock::CutoffLease;
    if(jobs || !proof.quiescent)return CloseBlock::Consumer;
    actions.release_inputs();
    actions.request_model();
    proof.requested=true;
    proof.closed=actions.model_closed();
    if(proof.hasModel)proof.model=actions.model_receipt();
    if(!proof.closed) {
        actions.retry_model();
        const auto block=close_generation(proof);
        return block==CloseBlock::None?CloseBlock::ModelSettlement:block;
    }
    if(proof.hasModel)if(const auto block=close_model(proof.model);block!=CloseBlock::None)return block;
    proof.bridge=actions.close_bridge()?CloseResult::Complete:CloseResult::Refused;
    if(proof.bridge!=CloseResult::Complete)return CloseBlock::Bridge;
    proof.generation=actions.complete_generation()?CloseResult::Complete:CloseResult::Refused;
    if(proof.generation!=CloseResult::Complete)return CloseBlock::Generation;
    for(auto& slot:proof.slots)slot.occupied=false;
    return close_generation(proof);
}
template<class Actions> bool close_pipeline_steps(CloseSnapshot& proof,Actions& actions)
{
    if(proof.closed)return true;
    const bool closed=actions.try_close();
    proof.pipeline=closed?CloseResult::Complete:CloseResult::Refused;
    if(!closed){proof.block=CloseBlock::Pipeline;return false;}
    actions.release();
    proof.closed=true;proof.workerExited=true;proof.block=CloseBlock::None;
    proof.refusingGeneration=proof.refusingSerial=0;proof.refusingSlot=UINT32_MAX;
    return true;
}
inline bool phase_accepted(bool native,bool requiresPlaceholder,bool placeholder,
    bool genuine,bool cold,bool settingsApplied)
{
    if(!settingsApplied)return false;
    if(requiresPlaceholder)return placeholder;
    return !placeholder && (!native || (genuine && !cold));
}
inline bool observation_accepted(ObservationPath path,bool native,bool requiresPlaceholder,
    bool placeholder,bool genuine,bool cold,bool settingsApplied) {
    return path!=ObservationPath::ColdCurrentSource && path!=ObservationPath::Reject &&
        phase_accepted(native,requiresPlaceholder,placeholder,genuine,cold,settingsApplied);
}
inline bool replacement_instance(uint64_t replaced,uint64_t current)
{
    return replaced && current && replaced!=current;
}
struct ReplacementBaseline {
    uint64_t original=0;
    void retire(uint64_t value) {original=value;}
    bool accepts(uint64_t current) const {return replacement_instance(original,current);}
};
inline bool settings_applied(uint64_t required,uint64_t applied,std::optional<uint64_t> selected)
{
    return applied>=required && (!selected || *selected>=required);
}
class CloseSnapshotStore {
    CloseSnapshot snapshot_{};
public:
    void record(const CloseSnapshot& snapshot) {snapshot_=snapshot;}
    CloseSnapshot read(uint64_t retiring=0) const {
        auto snapshot=snapshot_;snapshot.retiringCount=retiring;return snapshot;
    }
};
struct Observation {
    const uint64_t canonicalTick; // Supplied OBS token, never a source timestamp.
    const uint64_t sourceIdentity, deviceIdentity; // Lifetime tokens, not object addresses.
    const SourceValidity validity;
    const FrameDescriptor descriptor;
    const Settings settings;
};
struct Decision {
    const Epochs epochs;
    const std::optional<ResetCause> cause;
    const bool resetTemporal; // Both NR and OFA; not a physical completion receipt.
    const std::optional<Selection> placeholder;
};
// Two independent raw/view leases per physical generation. A pending cutoff
// forbids new capture; reentry must not replace it or infer fence completion.
class SnapshotRetirement final {
    const uint64_t generation_;
    uint64_t fence_ = 0;
    std::array<std::shared_ptr<const void>,2> held_;
public:
    explicit SnapshotRetirement(uint64_t generation) : generation_(generation) {}
    bool retire(const std::array<std::shared_ptr<const void>,2>&,uint64_t submittedFence);
    bool collect(uint64_t generation,uint64_t completedFence);
    bool capture_allowed() const { return !held_[0] && !held_[1]; }
    uint32_t leases() const { return static_cast<uint32_t>(!!held_[0])+static_cast<uint32_t>(!!held_[1]); }
};
// One leaf per accepting FramePipeline, serialized by its graphics owner.
// First valid observation describes the resources already created for that
// generation. Call before immutable ingress construction and begin, once per
// canonical observation. A placeholder MUST bypass capture/submission and latch
// current-source drawing in the adapter; never use old view/raw bindings.
class Observer final {
    pipeline::FramePipeline& pipeline_;
    std::optional<Observation> configured_;
    bool invalid_ = false, blocked_ = false;
public:
    explicit Observer(pipeline::FramePipeline& pipeline) : pipeline_(pipeline) {}
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;
    Decision prepare(const Observation&, bool cut = false, Reason boundaryProblem = Reason::None);
    // begin/finish already reset missed/skipped/failed evaluation histories.
    // The native caller must also reconcile those resulting epochs/resetTemporal;
    // no duplicate scheduler or synthetic ownership completion lives here.
};
} // namespace nr::lifecycle
