// SPDX-License-Identifier: GPL-3.0-only
// CPU-only contract tests for the host teardown-order seams:
//  1. exact custody of a queued non-blocking activation task (timeout is not cancellation);
//  2. the retirement gate that keeps every owner alive while the exact encoder stop is unresolved;
//  3. releasing the local Bridge lease before the cooperative stop entry point;
//  4. releasing the exit-signal/future hold immediately after the final Close confirmation.
#include "../host/nr_recording_cleanup.h"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
using namespace nr::qa;
namespace {
unsigned checks=0;
void expect(bool value,const char* message) { ++checks;if(!value)throw std::runtime_error(message); }
// The exact owner of one lane activation. A late queued task must act on live memory and
// must never run after the completion it publishes has been observed.
struct FakeTarget {
    bool alive=true,started=false,registered=false,resetInactive=false,useAfterReset=false;
    void activation() {
        if(!alive || resetInactive) { useAfterReset=true;return; }
        started=true;registered=true;
    }
};
struct FakeLease {
    bool live=true;
    unsigned releases=0;
    void reset() {if(live){live=false;++releases;}}
};
void activation_custody()
{
    QueuedTaskCustody custody;FakeTarget target;
    custody.begin(); // obs_queue_task accepted the activation task.
    // The caller's finite Event wait ended without observing the completion: a timeout is
    // not a cancellation, so the task still owns the target and nothing here may close,
    // reset or destroy the Recording, the Harness or the raw registration.
    expect(custody.must_retain(),"unobserved queued activation retains its owners");
    expect(!custody.observed(),"timeout must not report the activation as observed");
    expect(target.alive,"timed-out activation keeps the target alive");
    if(!custody.must_retain()) {target.resetInactive=true;target.alive=false;} // the pre-fix unwind
    expect(custody.act(),"held activation task acts exactly once");
    target.activation();
    custody.complete();
    expect(target.alive && target.started && target.registered,
        "late activation registered its raw callback on the retained live target");
    expect(!target.useAfterReset,"late activation never touches a reset recording");
    expect(custody.observed() && !custody.must_retain(),
        "published completion releases the retention obligation");
    expect(!custody.act(),"task never acts again after publishing its completion");
    // Second lane: an observed activation needs no retention and still acts once.
    QueuedTaskCustody second;second.begin();
    expect(second.act(),"queued task acts once");second.complete();
    expect(second.observed() && !second.must_retain(),"observed activation needs no retention");
    expect(!second.act(),"observed activation is not repeated");
}
void retirement_gate()
{
    const auto unresolved=recording_retirement(true,true,true);
    expect(unresolved.retained,"unresolved exact encoder stop retains the lane");
    expect(!unresolved.removeRawCallback && !unresolved.stopGraph && !unresolved.destroyGraph,
        "unresolved stop removes no raw callback and stops/destroys no graph");
    const auto resolved=recording_retirement(false,true,true);
    expect(!resolved.retained && resolved.removeRawCallback && resolved.stopGraph && resolved.destroyGraph,
        "resolved stop removes the raw callback then stops and destroys the graph");
    const auto bare=recording_retirement(false,false,false);
    expect(!bare.retained && !bare.removeRawCallback && !bare.stopGraph && !bare.destroyGraph,
        "resolved stop without a registered seam performs no cleanup step");
}
void stop_lease_order()
{
    FakeLease lease;bool entered=false;
    enter_stop_released(lease,[&] {expect(!lease.live,"local lease is released before the stop entry point");entered=true;});
    expect(entered && !lease.live && lease.releases==1,"stop entry ran after exactly one lease release");
}
void exit_hold_release()
{
    auto hold=std::make_shared<int>(7);
    expect(!release_after_final_close(hold,false) && hold,"unconfirmed close keeps the exit hold");
    expect(release_after_final_close(hold,true) && !hold,"final Close confirmation releases the exit hold");
}
}
int main(int argc,char** argv) try {
    if(argc>1 && std::string(argv[1])=="retirement") {
        retirement_gate();
        std::printf("RECORDING_RETIREMENT_GATE checks=%u unresolved_retains=true\n",checks);
        return 0;
    }
    if(argc>1 && std::string(argv[1])=="stop-order") {
        stop_lease_order();exit_hold_release();
        std::printf("RECORDING_STOP_ORDER checks=%u lease_before_stop=true exit_released_on_close=true\n",checks);
        return 0;
    }
    activation_custody();retirement_gate();stop_lease_order();exit_hold_release();
    std::printf("RECORDING_ACTIVATION_CUSTODY checks=%u timeout_retains=true late_act_once=true\n",checks);
    return 0;
}catch(const std::exception& e) { std::printf("FAIL %s\n",e.what());return 1; }
