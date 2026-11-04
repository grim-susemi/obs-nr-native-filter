// SPDX-License-Identifier: GPL-3.0-only
// CPU-only checks for cost completion receipts and exact owner custody.
#include "../host/nr_cost_completion.h"
#include "../host/nr_model_ownership.h"
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
using namespace nr;
using namespace nr::qa;
namespace {
void expect(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct FakeOwner { static unsigned destroyed; ~FakeOwner(){++destroyed;} };
unsigned FakeOwner::destroyed=0;
}
int main() try {
    // Given: a submitted evaluation with a signalled fence and settled production submission.
    CostCompletionFacts good{CostStage::Evaluation,1,true,S_OK,WAIT_OBJECT_0,std::nullopt,
        1,runtime::Completion::Complete,S_OK};
    // When: the production completion decision evaluates it.
    // Then: exact physical completion is established.
    expect(cost_completion_confirmed(good),"confirmed completion refused");
    auto failed=good; failed.fenceValue=UINT64_MAX;
    failed.submission=runtime::Completion::SignalFailed;
    failed.deviceRemoved=DXGI_ERROR_DEVICE_REMOVED;
    expect(!cost_completion_confirmed(failed),"removed device accepted");
    auto timeout=good; timeout.waitResult=WAIT_TIMEOUT; timeout.fenceValue=0;
    expect(!cost_completion_confirmed(timeout),"timeout accepted");
    auto signal=good; signal.queueSignal=E_FAIL; signal.waitResult=std::nullopt;
    expect(!cost_completion_confirmed(signal),"failed signal accepted");
    auto submission=good; submission.submissionAccepted=false;
    submission.queueSignal=std::nullopt; submission.waitResult=std::nullopt;
    submission.submission=runtime::Completion::SignalFailed;
    expect(!cost_completion_confirmed(submission),"failed production submission accepted");

    // Given: an observed failure with distinct machine values for every field.
    failed.waitResult=WAIT_FAILED; failed.waitError=1234;
    std::ostringstream out;
    // When: the same writer used by the host serializes the receipt.
    write_cost_completion(out,failed);
    // Then: the individual diagnostics remain machine distinguishable.
    const auto text=out.str();
    for(const char* field:{"\"stage\":\"evaluation\"","\"frame\":1","\"submission_accepted\":true",
        "\"queue_signal_hresult\":0","\"wait_result\":4294967295","\"wait_error\":1234",
        "\"fence_value\":18446744073709551615","\"submission_state\":\"signal_failed\"",
        "\"device_removed_hresult\":2289696773"})
        expect(text.find(field)!=std::string::npos,"missing diagnostic field");
    std::ostringstream absent;
    submission.stage=CostStage::Prepare; submission.frame=0;
    write_cost_completion(absent,submission);
    expect(absent.str().find("\"stage\":\"prepare\",\"frame\":0")!=std::string::npos,"missing prepare stage");
    expect(absent.str().find("\"queue_signal_hresult\":null")!=std::string::npos,"missing unattempted signal");
    expect(absent.str().find("\"wait_result\":null")!=std::string::npos,"missing unattempted wait");

    // Given: the same exact owner handoff used by the host after unresolved completion.
    auto owner=std::make_unique<FakeOwner>();
    const auto prior=ModelOwnership::retained();
    // When: physical completion is unresolved.
    if(!cost_completion_confirmed(failed)) retain_unresolved_model(owner);
    // Then: the owner remains in process-lifetime custody and cannot be destroyed locally.
    expect(!owner && FakeOwner::destroyed==0 && ModelOwnership::retained()==prior+1,
        "unresolved owner was not retained");
    std::puts("PASS cost_completion CPU_only=true");
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL cost_completion %s\n",error.what());
    return 1;
}
