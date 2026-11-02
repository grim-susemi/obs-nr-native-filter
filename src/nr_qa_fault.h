// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <windows.h>
#include "nr_contract.h"
#include <atomic>
namespace nr::model {
enum class Fault { None, Capability, Creation, Evaluation, Submission, AfterCoreInit,
    ProducerTimeout, ProducerRemoved, CompletionSignal };
// Adapter observations only. Neither helper writes a fence or ownership receipt.
// The same bounded producer-return seam is used by production and CPU tests.
template<class Wait>
uint32_t producer_return(Fault fault,uint64_t required,uint64_t actual,Wait&& wait) {
    const auto observed=fault==Fault::ProducerRemoved?UINT64_MAX:actual;
    if(observed==UINT64_MAX)return 0xe004;
    if(fault==Fault::ProducerTimeout)return 0xe005;
    if(observed<required && !wait())return 0xe005;
    return 0;
}
template<class Signal>
HRESULT completion_signal(Fault fault,Signal&& signal) {
    // A failed return is sticky. No later call changes it to success or signals
    // the missing marker. The original callback/fence owners remain retained.
    return fault==Fault::CompletionSignal?E_FAIL:signal();
}
// One exact controlled omission, not a retry facility. No fence or readiness
// is stored here; genuine original completion remains outside this state.
class OriginalMarker {
public:
    enum class State : uint32_t { Unavailable, Held, Requested, Attempted, Submitted, Failed };
private:
    const JobKey key_;
    std::atomic<State> state_{State::Unavailable};
    std::atomic<HRESULT> result_{E_PENDING};
    std::atomic<uint32_t> calls_{0},thread_{0};
public:
    explicit OriginalMarker(const JobKey& key):key_(key){}
    State state() const noexcept { return state_.load(); }
    HRESULT result() const noexcept { return result_.load(std::memory_order_acquire); }
    uint32_t calls() const noexcept { return calls_.load(std::memory_order_acquire); }
    uint32_t thread() const noexcept { return thread_.load(std::memory_order_acquire); }
    bool hold(Fault fault,bool submitted,bool healthy) noexcept {
        if(fault!=Fault::CompletionSignal || !submitted || !healthy)return false;
        auto expected=State::Unavailable;
        return state_.compare_exchange_strong(expected,State::Held);
    }
    bool request(const JobKey& key) noexcept {
        if(key!=key_)return false;
        auto expected=State::Held;
        return state_.compare_exchange_strong(expected,State::Requested);
    }
    template<class Healthy,class Signal>
    bool consume(const JobKey& key,Healthy&& healthy,Signal&& signal) noexcept {
        if(key!=key_)return false;
        auto expected=State::Requested;
        if(!state_.compare_exchange_strong(expected,State::Attempted))return false;
        // Consume BEFORE checking/calling; failed health/call cannot try again.
        HRESULT result=E_FAIL;
        if(healthy()) {
            thread_.store(GetCurrentThreadId(),std::memory_order_relaxed);
            calls_.fetch_add(1,std::memory_order_relaxed);
            result=signal(uint64_t{1});
        }
        result_.store(result,std::memory_order_relaxed);
        state_.store(SUCCEEDED(result)?State::Submitted:State::Failed);
        return true;
    }
};
inline bool original_completion_allowed(Fault fault,OriginalMarker::State state,uint64_t actual,HRESULT signalResult=S_OK) {
    return SUCCEEDED(signalResult) && actual!=UINT64_MAX && actual>=1 &&
        (fault!=Fault::CompletionSignal || state==OriginalMarker::State::Submitted);
}
inline bool qa_publication_allowed(uint32_t failure) noexcept { return failure==0; }
// Narrow fixture ordering seam, shared with its CPU ordering regression. These
// are the actual subscription/render/snapshot boundaries, not a second host.
template<class Subscribe,class Render,class Snapshot>
bool initialize_fault_fixture(Subscribe&& subscribe,Render&& render,Snapshot&& snapshot) {
    if(!subscribe())return false;
    render();
    return snapshot();
}
}
