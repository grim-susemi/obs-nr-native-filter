// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_runtime.h"
#include <optional>
#include <ostream>

namespace nr::qa {
enum class CostStage { Prepare, Evaluation };
struct CostCompletionFacts {
    CostStage stage;
    unsigned frame;
    bool submissionAccepted;
    std::optional<HRESULT> queueSignal;
    std::optional<DWORD> waitResult;
    std::optional<DWORD> waitError;
    uint64_t fenceValue;
    runtime::Completion submission;
    HRESULT deviceRemoved;
};
inline bool cost_completion_confirmed(const CostCompletionFacts& facts) noexcept {
    return facts.submissionAccepted && facts.queueSignal && SUCCEEDED(*facts.queueSignal) &&
        facts.waitResult==WAIT_OBJECT_0 && facts.fenceValue!=UINT64_MAX && facts.fenceValue>=1 &&
        facts.submission==runtime::Completion::Complete;
}
inline const char* cost_submission_name(runtime::Completion state) noexcept {
    switch(state) {
    case runtime::Completion::Recorded: return "recorded";
    case runtime::Completion::Submitted: return "submitted";
    case runtime::Completion::SignalFailed: return "signal_failed";
    case runtime::Completion::Complete: return "complete";
    case runtime::Completion::Discarded: return "discarded";
    }
    return "invalid";
}
inline void write_cost_completion(std::ostream& out,const CostCompletionFacts& facts) {
    out<<"NR_COST_COMPLETION_FAILURE {\"stage\":\"";
    switch(facts.stage) {
    case CostStage::Prepare: out<<"prepare"; break;
    case CostStage::Evaluation: out<<"evaluation"; break;
    }
    out<<"\",\"frame\":"<<facts.frame<<",\"submission_accepted\":"<<(facts.submissionAccepted?"true":"false")
        <<",\"queue_signal_hresult\":";
    if(facts.queueSignal) out<<static_cast<uint32_t>(*facts.queueSignal); else out<<"null";
    out<<",\"wait_result\":";
    if(facts.waitResult) out<<static_cast<uint32_t>(*facts.waitResult); else out<<"null";
    out<<",\"wait_error\":";
    if(facts.waitError) out<<static_cast<uint32_t>(*facts.waitError); else out<<"null";
    out<<",\"fence_value\":"<<facts.fenceValue<<",\"submission_state\":\""
        <<cost_submission_name(facts.submission)<<"\",\"device_removed_hresult\":"
        <<static_cast<uint32_t>(facts.deviceRemoved)<<"}"<<std::endl;
}
}
