// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include "recording_packet_transport.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
namespace nr::qa {
enum class RecordingBranch { Inactive,RawOnly,Shared,Unresolved };
struct FixtureProducerContract {
    const FrameKey key;
    const FrameDescriptor descriptor;
    const uint64_t sourceIdentity,deviceIdentity;
    const bool nativeKeyAbsent;
    bool operator==(const FixtureProducerContract&) const = default;
};
struct RecordingProducer {
    const std::optional<IngressSample> selected,current;
    const std::optional<Selection> selection;
    const std::optional<JobContract> job;
    const std::optional<FixtureProducerContract> fixture;
    const uint32_t content;
    const bool readComplete,resourceAgreement,completionAgreement;
};
struct RecordingRender {
    const uint64_t epoch,occurrence,clock,predecessor;
    const RecordingBranch branch;
    const bool firstActive;
    const std::shared_ptr<const RecordingProducer> producer;
};
struct RecordingDelivery {
    const uint64_t epoch,occurrence,timestamp;
    const std::array<uintptr_t,2> planes;
    std::optional<size_t> render;
    uint32_t repetition=0;
    bool unresolved=false;
};
struct RecordingCounters {
    uintptr_t root;
    uint32_t skipped,lagged;
};
template<class... Owners>
void retire_recording_producer(bool readComplete,bool published,bool graphicsOwner,Owners&... owners)
{
    if(!readComplete || !published || !graphicsOwner)
        throw std::runtime_error("recording premature or foreign producer retirement");
    (owners.reset(),...);
}
// The runner calls this seam only while holding its existing lane mutex.
// It contains no GPU/resource leases and no content-assisted selector.
class RecordingOccurrences {
    uint64_t epoch_=0,interval_=0;
    uint32_t rateNum_=0,rateDen_=0;
    bool countersClosed_=false,contaminated_=false;
    RecordingCounters baseline_{};
    void match(size_t index) {
        auto& delivery=deliveries[index];
        if(delivery.render || delivery.unresolved)return;
        if(delivery.repetition) {
            if(index && deliveries[index-1].render)delivery.render=deliveries[index-1].render;
            return;
        }
        size_t found=0,count=0;
        for(size_t i=0;i<renders.size();++i) {
            const auto& render=renders[i];
            if(render.epoch!=delivery.epoch ||
                (render.firstActive && render.branch==RecordingBranch::Shared))continue;
            const bool matches=render.branch==RecordingBranch::RawOnly?render.clock==delivery.timestamp:
                render.branch==RecordingBranch::Shared && render.predecessor==delivery.timestamp;
            if(matches){found=i;++count;}
        }
        if(count>1)throw std::runtime_error("recording ambiguous render receipt");
        if(!count)return;
        if(index && deliveries[index-1].render && found<=*deliveries[index-1].render)
            throw std::runtime_error("recording reordered render receipt");
        delivery.render=found;
    }
public:
    static constexpr size_t bound=32;
    std::vector<RecordingRender> renders;
    std::vector<RecordingDelivery> deliveries;
    void begin(uint64_t epoch,uint32_t numerator,uint32_t denominator,RecordingCounters counters) {
        if(!epoch || !numerator || !denominator || !counters.root ||
            (!deliveries.empty() && (!countersClosed_ ||
                std::any_of(deliveries.begin(),deliveries.end(),[](const auto& d){return !d.render;}))))
            throw std::runtime_error("recording reset with unresolved causal epoch");
        if(epoch_ && epoch<=epoch_)throw std::runtime_error("recording nonadvancing transport epoch");
        epoch_=epoch;rateNum_=numerator;rateDen_=denominator;
        interval_=1000000000ull*denominator/numerator;baseline_=counters;
        countersClosed_=contaminated_=false;renders.clear();deliveries.clear();
    }
    uint64_t interval() const {return interval_;}
    void publish(RecordingRender render) {
        if(renders.size()==bound)throw std::runtime_error("recording render receipt overflow");
        if(render.epoch!=epoch_ || !render.occurrence || !render.clock || !render.producer ||
            !render.producer->readComplete || !render.producer->resourceAgreement ||
            !render.producer->completionAgreement)
            throw std::runtime_error("recording incomplete immutable producer receipt");
        if(!renders.empty() && (render.occurrence!=renders.back().occurrence+1 ||
            render.predecessor!=renders.back().clock || render.clock-render.predecessor!=interval_))
            throw std::runtime_error("recording duplicate missing or lagged graphics occurrence");
        if(render.producer->job && (!render.producer->selected || !render.producer->selection ||
            render.producer->job->input!=*render.producer->selected ||
            render.producer->job->key.frame!=render.producer->selected->key ||
            render.producer->selection->job!=render.producer->job->key ||
            render.producer->selection->frame!=render.producer->selected->key ||
            render.producer->job->key.resourceGeneration!=render.producer->selected->resourceGeneration))
            throw std::runtime_error("recording immutable selected job mismatch");
        if(render.producer->selected && render.producer->current &&
            (render.producer->selected->key.instanceId!=render.producer->current->key.instanceId ||
                render.producer->selected->key.contentEpoch!=render.producer->current->key.contentEpoch ||
                render.producer->selected->sourceIdentity!=render.producer->current->sourceIdentity ||
                render.producer->selected->deviceIdentity!=render.producer->current->deviceIdentity ||
                render.producer->selected->resourceGeneration!=render.producer->current->resourceGeneration ||
                render.producer->selected->descriptor!=render.producer->current->descriptor ||
                render.producer->selected->settings!=render.producer->current->settings ||
                !render.producer->selection ||
                render.producer->selection->canonicalTick!=render.producer->current->canonicalTick ||
                (render.producer->selection->frame &&
                    render.producer->selection->frame!=render.producer->selected->key)))
            throw std::runtime_error("recording producer source format settings generation or selection mismatch");
        if(render.producer->selected && (!render.producer->selected->key.valid() ||
            !render.producer->selected->descriptor.valid() || !render.producer->selected->settings.valid() ||
            render.producer->selected->validity!=SourceValidity::Valid))
            throw std::runtime_error("recording invalid immutable producer input");
        if(render.producer->selection &&
            ((render.producer->selection->kind==OutputKind::DelayedNr)!=render.producer->job.has_value() ||
                (render.producer->job && (render.producer->selection->status!=Status::Applied ||
                    render.producer->selection->reason!=Reason::None))))
            throw std::runtime_error("recording selected status or job presence mismatch");
        if(render.producer->fixture && (render.producer->selected || render.producer->job ||
            !render.producer->fixture->key.valid() || !render.producer->fixture->descriptor.valid() ||
            !render.producer->fixture->sourceIdentity || !render.producer->fixture->deviceIdentity ||
            !render.producer->fixture->nativeKeyAbsent))
            throw std::runtime_error("recording fabricated OFF native key");
        renders.push_back(std::move(render));
        for(size_t i=0;i<deliveries.size();++i)match(i);
    }
    size_t deliver(uint64_t epoch,uint64_t timestamp,std::array<uintptr_t,2> planes,bool noScaler) {
        if(deliveries.size()==bound)throw std::runtime_error("recording raw receipt overflow");
        if(epoch!=epoch_ || !timestamp || !planes[0] || !planes[1] || !noScaler)
            throw std::runtime_error("recording raw epoch planes or conversion mismatch");
        if(!deliveries.empty() && timestamp-deliveries.back().timestamp!=interval_)
            throw std::runtime_error("recording duplicate missing or reordered raw occurrence");
        const auto index=deliveries.size();
        RecordingDelivery value{epoch,index+1,timestamp,planes,{},0,false};
        // Only consecutive pointer equality proves a run in the unscaled six-entry cache.
        if(index && planes==deliveries.back().planes) {
            value.render=deliveries.back().render;
            value.repetition=deliveries.back().repetition+1;
            value.unresolved=false;
            contaminated_=true;
        }
        deliveries.push_back(value);match(index);return index;
    }
    void close_counters(RecordingCounters end) {
        if(end.root!=baseline_.root || end.skipped<baseline_.skipped || end.lagged<baseline_.lagged)
            throw std::runtime_error("recording counter root reset or wrap");
        contaminated_=contaminated_ || end.skipped!=baseline_.skipped || end.lagged!=baseline_.lagged;
        countersClosed_=true;
    }
    std::vector<size_t> packets(const std::vector<PacketTransportReceipt>& packets) const {
        std::vector<size_t> completeCandidates;
        for(size_t i=0;i<renders.size();++i)
            if(!renders[i].firstActive && (renders[i].branch==RecordingBranch::Shared ||
                renders[i].branch==RecordingBranch::Unresolved))completeCandidates.push_back(i);
        if(!countersClosed_ || contaminated_ || packets.empty() || packets.size()>bound ||
            completeCandidates.empty())
            throw std::runtime_error("recording contaminated or incomplete transport epoch");
        std::vector<uint64_t> floors;
        size_t start=0,count=0;
        for(size_t slot=0;slot<completeCandidates.size();++slot) {
            const auto index=completeCandidates[slot];
            if(index>=renders.size())throw std::runtime_error("recording incomplete encoder start candidates");
            const auto& render=renders[index];
            if(!render.predecessor)
                throw std::runtime_error("recording unresolved encoder input branch");
            const auto floor=render.predecessor/1000;
            if(std::find(floors.begin(),floors.end(),floor)!=floors.end())
                throw std::runtime_error("recording ambiguous encoder start floors");
            floors.push_back(floor);
            if(packets.front().systemDtsUsec>=0 && floor==static_cast<uint64_t>(packets.front().systemDtsUsec))
                {start=slot;++count;}
        }
        if(count!=1 || start+packets.size()>completeCandidates.size() ||
            packets.front().pts!=0 || packets.front().dts!=0)
            throw std::runtime_error("recording exact encoder input start unresolved");
        std::vector<size_t> result;
        for(size_t i=0;i<packets.size();++i) {
            const auto& packet=packets[i];
            const auto index=completeCandidates[start+i];
            const auto expectedDts=packets.front().dtsUsec+
                static_cast<int64_t>(1000000ull*i*rateDen_/rateNum_);
            if(renders[index].branch!=RecordingBranch::Shared ||
                packet.occurrence!=i || packet.pts!=static_cast<int64_t>(i)*rateDen_ ||
                packet.dts!=packet.pts || packet.timebaseNumerator!=static_cast<int32_t>(rateDen_) ||
                packet.timebaseDenominator!=static_cast<int32_t>(rateNum_) ||
                !packet.bytes || !packet.ownedEncoder || !packet.singlePicture ||
                packet.dtsUsec!=expectedDts || packet.systemDtsUsec!=packet.dtsUsec ||
                packet.keyframe!=packet.idrPicture || (!i && !packet.keyframe) ||
                (i && index!=completeCandidates[start+i-1]+1))
                throw std::runtime_error("recording packet order count timebase or pause mismatch");
            const auto before=std::find_if(deliveries.begin(),deliveries.end(),[&](const RecordingDelivery& d) {
                return d.render==index && !d.repetition && !d.unresolved;
            });
            if(before==deliveries.end())throw std::runtime_error("recording packet lacks exact raw occurrence");
            result.push_back(static_cast<size_t>(before-deliveries.begin()));
        }
        return result;
    }
};
}
