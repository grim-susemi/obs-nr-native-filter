// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "recording_program_marker.h"
#include "recording_occurrence.h"
namespace nr::qa {
struct ProgramPhysicalReceipt {
    const std::optional<JobKey> completedTicket,copiedTicket;
    const uint32_t slot;
    const uint64_t producer,worker,consumer,producerDone,workerDone,consumerDone;
    const uintptr_t inputResource,completedOutput,selectedTarget,programTarget;
    const bool copyPublished;
};
struct ProgramUseReceipt {
    const uint32_t wire,comparisonEpoch;
    const uint64_t occurrence,clock;
    const std::shared_ptr<const RecordingProducer> producer;
    const ProgramPhysicalReceipt physical;
};
struct ProgramDeliveryReceipt {
    const uint32_t wire;
    const uint64_t occurrence,timestamp;
    const std::array<uintptr_t,2> planes;
    const size_t use;
    const uint32_t repetition;
};
inline void validate_program_physical(const RecordingProducer& producer,const ProgramPhysicalReceipt& physical) {
    if(!producer.readComplete || !producer.resourceAgreement || !producer.completionAgreement ||
        !physical.inputResource || !physical.selectedTarget || !physical.programTarget || !physical.copyPublished)
        throw std::runtime_error("program witness incomplete physical pixel path");
    if(producer.fixture) {
        if(producer.selected || producer.job || physical.completedTicket || physical.copiedTicket ||
            !producer.fixture->nativeKeyAbsent)
            throw std::runtime_error("program witness fabricated OFF ticket");
        return;
    }
    if(!producer.selected || !producer.current || !producer.selection ||
        producer.selection->canonicalTick!=producer.current->canonicalTick ||
        (producer.selection->frame && producer.selection->frame!=producer.selected->key) ||
        producer.selected->settings!=producer.current->settings ||
        producer.selected->descriptor!=producer.current->descriptor ||
        producer.selected->resourceGeneration!=producer.current->resourceGeneration)
        throw std::runtime_error("program witness immutable input/selection mismatch");
    const bool nr=producer.selection->kind==OutputKind::DelayedNr;
    if(nr) {
        if(!producer.job || producer.job->input!=*producer.selected ||
            producer.selection->job!=producer.job->key ||
            physical.completedTicket!=producer.job->key || physical.copiedTicket!=producer.job->key ||
            physical.slot>=3 || !physical.completedOutput || !physical.producer || !physical.worker || !physical.consumer ||
            physical.producerDone<physical.producer || physical.workerDone<physical.worker ||
            physical.consumerDone<physical.consumer || physical.producerDone==UINT64_MAX ||
            physical.workerDone==UINT64_MAX || physical.consumerDone==UINT64_MAX)
            throw std::runtime_error("program witness stale/wrong completed output ticket");
    }else if(producer.job || physical.completedTicket || physical.copiedTicket)
        throw std::runtime_error("program witness raw path carries NR output");
}
// Lane-local immutable archive. Only transported picture codes choose a use.
class ProgramWitness {
    uint32_t epoch_=0;
public:
    static constexpr size_t bound=32;
    std::vector<ProgramUseReceipt> uses;
    std::vector<ProgramDeliveryReceipt> deliveries;
    void begin(uint32_t epoch) {
        if(!epoch || (epoch_ && epoch<=epoch_))
            throw std::runtime_error("program witness reset epoch");
        uses.reserve(bound);deliveries.reserve(bound);
        epoch_=epoch;uses.clear();deliveries.clear();
    }
    void seal(ProgramUseReceipt receipt) {
        if(uses.size()==bound || receipt.comparisonEpoch!=epoch_ || !receipt.producer ||
            !receipt.wire || !receipt.occurrence ||
            (!uses.empty() && receipt.occurrence!=uses.back().occurrence+1))
            throw std::runtime_error("program witness missing/extra use");
        if(std::any_of(uses.begin(),uses.end(),[&](const auto& value){return value.wire==receipt.wire;}))
            throw std::runtime_error("program witness duplicate wire aliases actual uses");
        validate_program_physical(*receipt.producer,receipt.physical);
        const auto& p=*receipt.producer;
        const auto ingress=p.fixture?p.fixture->key.ingressSequence:p.selected->key.ingressSequence;
        const auto phase=p.fixture?0u:p.selection->kind==OutputKind::PrimingRaw?1u:
            p.selection->kind==OutputKind::CurrentSource?2u:0u;
        if(ingress>256 || receipt.wire!=ProgramMarker::wire(epoch_,static_cast<uint32_t>(ingress),phase))
            throw std::runtime_error("program witness wire does not name sealed physical use");
        uses.push_back(std::move(receipt));
    }
    size_t use(uint32_t wire) const {
        const auto found=std::find_if(uses.begin(),uses.end(),[&](const auto& value){return value.wire==wire;});
        if(found==uses.end())throw std::runtime_error("program witness unknown/cross-epoch wire");
        return static_cast<size_t>(found-uses.begin());
    }
    template<class Code>
    size_t raw(const DecodedFrame& frame,std::array<uintptr_t,2> planes,Code code) {
        if(deliveries.size()==bound)throw std::runtime_error("program witness raw overflow");
        const auto wire=ProgramMarker::read(frame,code);
        const auto index=use(wire);
        uint32_t repetition=0;
        if(!deliveries.empty()) {
            const auto& previous=deliveries.back();
            if(wire==previous.wire) {
                if(planes!=previous.planes)throw std::runtime_error("program witness duplicate noncache delivery");
                repetition=previous.repetition+1;
            }else if(index<=previous.use)
                throw std::runtime_error("program witness reordered delivery");
        }
        deliveries.push_back({wire,deliveries.size()+1,frame.timestamp,planes,index,repetition});
        return deliveries.size()-1;
    }
    template<class Code>
    size_t decoded(const DecodedFrame& frame,size_t ordinal,size_t count,Code code) const {
        if(!count || ordinal>=count)throw std::runtime_error("program witness decoded cardinality");
        const auto wire=ProgramMarker::read(frame,code);
        const auto index=use(wire);
        const auto found=std::find_if(deliveries.begin(),deliveries.end(),[&](const auto& d){return d.use==index;});
        if(found==deliveries.end() || found->repetition)
            throw std::runtime_error("program witness decoded picture missing exact raw partner");
        return static_cast<size_t>(found-deliveries.begin());
    }
    void healthy(std::span<const size_t> associations) const {
        if(associations.empty())throw std::runtime_error("program witness empty capture");
        for(size_t i=0;i<associations.size();++i) {
            const auto index=associations[i];
            if(index>=deliveries.size() || deliveries[index].repetition ||
                (i && deliveries[index].use!=deliveries[associations[i-1]].use+1))
                throw std::runtime_error("program witness healthy missing/repeated/reordered use");
        }
    }
};
}
