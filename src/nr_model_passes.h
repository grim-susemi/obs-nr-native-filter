// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <memory>
namespace nr::model {
struct PassStatus {
    uint32_t requested=1,effective=0,failedPass=0,code=0;
    bool operator==(const PassStatus&) const=default;
};
// Counts occupy disjoint two-bit fields; the complete vendor/core code is retained.
inline constexpr uint64_t pack_pass_status(PassStatus s) {
    return uint64_t{s.requested}|(uint64_t{s.effective}<<2)|(uint64_t{s.failedPass}<<4)|(uint64_t{s.code}<<8);
}
inline constexpr PassStatus unpack_pass_status(uint64_t bits) {
    return {static_cast<uint32_t>(bits&3),static_cast<uint32_t>((bits>>2)&3),
        static_cast<uint32_t>((bits>>4)&3),static_cast<uint32_t>(bits>>8)};
}
// Conservative Task12 policy, not a measured universal minimum or aspect-ratio guarantee.
inline constexpr bool work_admitted(Extent work) {return work.width>=480 && work.height>=270;}
inline constexpr uint32_t work_scale_refused=0xe021;
struct PassGate {
    uint32_t ready,target;
    std::optional<uint32_t> create; // Zero-based first absent history, only after ready predecessors.
    bool primed() const {return ready==target;}
};
struct PassCompletion {
    uint32_t completed,failedPass,code;
    bool settled,publishable; // Exact physical completion or unsubmitted discard, never API success alone.
};
// Worker-owned policy; runtime::Pass continues to own all features/recordings.
class PassChain {
    uint32_t requested_=1,effective_=0;
    std::array<std::optional<uint32_t>,3> failures_{};
public:
    void reset(uint32_t requested) {requested_=requested;effective_=0;failures_={};}
    PassStatus status() const {
        for(uint32_t i=1;i<requested_;++i) if(failures_[i])
            return {requested_,std::min(effective_,i),i+1,*failures_[i]};
        return {requested_,effective_,0,0};
    }
    uint32_t limit() const {const auto s=status();return s.failedPass?s.failedPass-1:requested_;}
    template<class Pass>
    PassGate gate(const std::array<std::unique_ptr<Pass>,3>& passes,uint64_t epoch) const {
        const auto target=limit();uint32_t ready=0;
        while(ready<target && passes[ready] && passes[ready]->ready(epoch))++ready;
        return {ready,target,ready<target && !passes[ready]?std::optional{ready}:std::nullopt};
    }
    bool settle(PassCompletion c) {
        if(!c.settled)return false;
        if(c.failedPass>1 && c.failedPass<=requested_ && !failures_[c.failedPass-1])
            failures_[c.failedPass-1].emplace(c.code);
        if(c.publishable) {
            const auto completed=std::min(c.completed,limit());
            effective_=effective_?std::min(effective_,completed):completed;
        } else effective_=std::min(effective_,limit());
        return true;
    }
};
// Caller must settle the last job first. A refusing feature retains its exact owner.
template<class Pass>
bool release_passes(std::array<std::unique_ptr<Pass>,3>& passes,uint32_t first=0) {
    for(size_t i=passes.size();i>first;--i) if(passes[i-1]) {
        if(!passes[i-1]->release())return false;
        passes[i-1].reset();
    }
    return true;
}
}
