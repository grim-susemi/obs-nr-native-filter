// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <string>
namespace nr::qa::classified {
struct AdmissionEvidence {
    uint64_t tick=0;
    std::optional<JobKey> key,previous;
    bool tickWitnessMatched=false,predecessorWitnessMatched=false,previousStateObserved=false,previousReady=false,previousEvaluated=false;
    uint64_t previousCompletion=0,previousCompleted=0;
};
struct ResetClass {
    std::string name="UNKNOWN",provenance="admission-derived",exactMember="UNKNOWN";
    uint64_t historyDelta=0,contentDelta=0;
};
inline ResetClass reset_class(const AdmissionEvidence& a) {
    ResetClass r;
    if(!a.tickWitnessMatched || !a.predecessorWitnessMatched || !a.key || !a.previous)return r;
    r.name="generation/other";
    const auto& k=*a.key;const auto& p=*a.previous;
    if(k.frame.instanceId!=p.frame.instanceId || k.resourceGeneration!=p.resourceGeneration ||
        k.frame.ingressSequence!=p.frame.ingressSequence+1 || k.historyEpoch<p.historyEpoch ||
        k.frame.contentEpoch<p.frame.contentEpoch)return r;
    r.historyDelta=k.historyEpoch-p.historyEpoch;r.contentDelta=k.frame.contentEpoch-p.frame.contentEpoch;
    if(r.contentDelta)r.name="content";
    else if(r.historyDelta)r.name="history-only";
    else {r.name="none";r.exactMember="none";}
    return r;
}
struct RowEvidence {
    bool referenceValid=false,genuine=false,module=false,operandsCaptured=false,processorReset=false;
    bool previousKeyAbsent=false,historyMismatch=false,contentMismatch=false,ingressMismatch=false;
    bool admissionMatched=false,predecessorWitnessMatched=false,previousUnevaluatedObserved=false;
    double delta=0,packingBound=0;
    uint64_t rawHash=0,filteredHash=0;
    ResetClass admission;
};
inline const char* refusal(const RowEvidence& r) {
    if(!r.genuine || !r.module)return "genuine model/module required";
    if(r.admission.provenance!="admission-derived" ||
        (r.admission.name=="none" ? r.admission.exactMember!="none" : r.admission.exactMember!="UNKNOWN"))
        return "NR_RECORDING_RESET_CLASS_RELABELED_AS_CAUSE";
    if(r.referenceValid) {
        if(!(r.delta>r.packingBound) || r.rawHash==r.filteredHash)return "model no-op or metadata-only success";
    } else {
        if(!(r.delta<=r.packingBound) || r.rawHash!=r.filteredHash)return "NR_RECORDING_IDENTITY_CHANGED_IMAGE";
        const bool disjunct=r.processorReset || r.previousKeyAbsent || r.historyMismatch || r.contentMismatch || r.ingressMismatch;
        if(!r.operandsCaptured || !disjunct || !r.admissionMatched || !r.predecessorWitnessMatched ||
            r.admission.name=="UNKNOWN" ||
            (r.admission.name=="none" && !r.previousUnevaluatedObserved))return "NR_RECORDING_IDENTITY_RESET_UNATTRIBUTED";
    }
    return nullptr;
}
struct Window {
    unsigned applied=0,consecutive=0,longest=0;
    const char* rowFailure=nullptr;
    void observe(bool delayedNr,const RowEvidence& r) {
        const auto* error=delayedNr?refusal(r):nullptr;
        if(error && !rowFailure)rowFailure=error;
        const bool validApplied=delayedNr && !error && r.referenceValid;
        consecutive=validApplied?consecutive+1:0;
        if(validApplied)++applied;
        if(consecutive>longest)longest=consecutive;
    }
    const char* refusal_at_end() const {
        if(rowFailure)return rowFailure;
        return longest>=2?nullptr:"NR_RECORDING_NO_VALID_REFERENCE_WITHIN_WINDOW";
    }
};
}

