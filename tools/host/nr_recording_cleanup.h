// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// CPU-only teardown-order seams for the host recording lanes. No libobs, no GPU: the host
// calls these decisions on the real path and the contract tests drive them with fakes, so
// the required order is observable without an encoder, a device or a worker thread.
#include <atomic>
namespace nr::qa {
// Exact custody of one queued non-blocking graphics task (Activate / DisconnectRaw).
// begin() runs on the control thread before obs_queue_task, act() inside the queued task,
// complete() on the control thread once the task's publication was actually observed.
// A finite Event wait that returned without observing that publication is NOT a
// cancellation: the obligation is still outstanding, the task still owns the target, and
// the Recording, the Harness and the raw registration must all be retained. act() also
// refuses to touch the target after the obligation was retired, so a late task can never
// register a raw callback or start an already reset recording.
class QueuedTaskCustody {
    enum State : int { Idle=0,Pending=1,Acted=2,Done=3 };
    std::atomic<int> state{Idle};
public:
    void begin() {state.store(Pending,std::memory_order_release);}
    bool act() {
        int expected=Pending;
        return state.compare_exchange_strong(expected,Acted,std::memory_order_acq_rel);
    }
    void complete() {state.store(Done,std::memory_order_release);}
    bool observed() const {return state.load(std::memory_order_acquire)==Done;}
    bool must_retain() const {
        const auto value=state.load(std::memory_order_acquire);
        return value==Pending || value==Acted;
    }
};
// Retirement gate for one lane after a failure. The exact encoder stop is decided first:
// while it is unresolved nothing is removed, stopped or destroyed, and the caller folds
// the retained lane into the registry main.cpp already merges into `retained`.
struct RecordingRetirement {
    bool retained,removeRawCallback,stopGraph,destroyGraph;
};
inline RecordingRetirement recording_retirement(bool stopUnresolved,bool rawConnected,bool graphLive)
{
    if(stopUnresolved)return {true,false,false,false};
    return {false,rawConnected,graphLive,graphLive};
}
// The local snapshot lease must not outlive the cooperative stop entry point: release it
// first, then enter. A lease held across the entry keeps the very owner under test alive.
template<class Lease,class StopEntry>
void enter_stop_released(Lease& lease,StopEntry&& entry)
{
    lease.reset();
    entry();
}
// The exit-signal/future hold is dropped on the final Close confirmation only.
template<class Hold>
bool release_after_final_close(Hold& hold,bool closed)
{
    if(!closed)return false;
    hold=Hold{};
    return true;
}
}
