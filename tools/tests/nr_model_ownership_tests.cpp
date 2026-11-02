// SPDX-License-Identifier: GPL-3.0-only
// H06 CPU seam: a fake Harness result proves the model lane always surfaces unresolved
// ownership to the coordinator, including the no-sample case where the retained HANDLE is
// null. No D3D12 device, no GPU, no OBS.
#include "../host/nr_model_ownership.h"
#include <cstdio>
#include <stdexcept>
#include <windows.h>
using namespace nr::qa;
namespace {
unsigned checks=0;
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
// Fake Harness result: exactly what nr_run_model_scenario knows when it cannot prove cleanup.
struct FakeHarnessResult { bool publishedSamples=false; bool samplesSettled=false; bool publishedRetainedHandle=false; };
struct FakeHarnessOwner {
    static unsigned destroyed;
    int graph=0;
    ~FakeHarnessOwner(){++destroyed;}
};
unsigned FakeHarnessOwner::destroyed=0;
// The exact contract under test: a lane that cannot prove cleanup must report unresolved.
bool lane_cleanup_settled(const FakeHarnessResult& result){
    return model_cleanup_settled(result.publishedSamples,result.samplesSettled);
}
}
int main() try {
    expect(!model_has_unresolved_owners(),"no retained model ownership before any failure");
    expect(core_close_admissible(),"CORE close is admissible while no readback owner is unresolved");
    // A fully published settled graph with no retained owner is the only normal path.
    expect(model_cleanup_settled(true,true),"a fully published settled graph with no retained owner settles");
    expect(!model_cleanup_settled(false,false),"an unpublished sample graph never settles");

    // H06(a): the failed lane retains its exact owner and the coordinator can observe it.
    const auto before=ModelOwnership::retained();
    auto first=std::make_unique<FakeHarnessOwner>();
    retain_unresolved_model(first);
    expect(!first,"unresolved model owner is not destroyed locally");
    expect(FakeHarnessOwner::destroyed==0,"unresolved model owner survived the lane scope");
    expect(model_has_unresolved_owners(),"model-side registry query reports unresolved ownership");
    expect(!core_close_admissible(),"CORE close steps are refused while a readback owner is unresolved");
    expect(ModelOwnership::retained()==before+1,"unresolved model owner counted exactly once");

    // H06(b): samples are empty, so no observed HANDLE exists. A null retained HANDLE is not
    // permission to reach obs_shutdown; the retained graph still reports unresolved.
    FakeHarnessResult empty;
    expect(!empty.publishedSamples && !empty.publishedRetainedHandle,"no sample published an observed HANDLE");
    expect(!lane_cleanup_settled(empty),"no-sample lane never takes the settled cleanup path");
    expect(fold_model_retained(false),"null retained HANDLE still folds into the retained flag");

    // H06(c): a settled sample graph is not enough while a host owner stays unresolved.
    FakeHarnessResult settledSamples;settledSamples.publishedSamples=true;settledSamples.samplesSettled=true;
    expect(!lane_cleanup_settled(settledSamples),"retained ownership blocks the settled cleanup path");

    // H06(d): a retained HANDLE existed but ownership is unresolved: same report.
    FakeHarnessResult published;published.publishedRetainedHandle=true;
    expect(!lane_cleanup_settled(published),"unresolved lane never takes the settled cleanup path");
    auto second=std::make_unique<FakeHarnessOwner>();
    retain_unresolved_model(second);
    expect(ModelOwnership::retained()==before+2,"second unresolved owner retained exactly once");
    expect(FakeHarnessOwner::destroyed==0,"retained model owners are never destroyed");
    expect(fold_model_retained(true),"existing retained reasons are preserved");

    std::printf("PASS model_ownership checks=%u retained=%zu CPU_only=true\n",checks,ModelOwnership::retained());
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL model_ownership %s checks=%u CPU_only=true\n",error.what(),checks);
    return 1;
}
