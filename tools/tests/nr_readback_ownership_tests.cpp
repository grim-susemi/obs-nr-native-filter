// SPDX-License-Identifier: GPL-3.0-only
// H05 CPU seam: fake queue/fence drive the exact readback-owner retention decision. No
// D3D12 device, no GPU, no OBS. The real nr_model_scenario.cpp readback() calls this helper.
#include "../host/nr_model_ownership.h"
#include <cstdio>
#include <stdexcept>
#include <windows.h>
using namespace nr::qa;
namespace {
unsigned checks=0;
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
// Fake submission peer: the exact ExecuteCommandLists/Signal pair the host uses.
struct FakeFence { uint64_t completed=0; };
struct FakeQueue {
    bool executed=false;
    void execute(){executed=true;}
    HRESULT signal(FakeFence& fence,uint64_t value){if(!executed)return E_FAIL;fence.completed=value;return S_OK;}
};
// Exact readback owner objects: target resource, allocator, command list, fence, event.
struct FakeReadbackOwner {
    static unsigned destroyed;
    int target=0,allocator=0,commands=0;
    FakeFence fence;
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    FakeReadbackOwner(){if(!event)throw std::runtime_error("fake readback event");}
    ~FakeReadbackOwner(){++destroyed;if(event)CloseHandle(event);}
};
unsigned FakeReadbackOwner::destroyed=0;
}
int main() try {
    // H05(a): ExecuteCommandLists ran, Signal failed. The exact owner objects (target,
    // allocator, command list, fence, event) must be retained for the process, not destroyed.
    FakeQueue queue;queue.execute();
    FakeFence failedFence;
    expect(queue.signal(failedFence,1)==S_OK,"fake submission signalled");
    auto failed=std::make_unique<FakeReadbackOwner>();
    expect(settle_readback_owner(true,E_FAIL,0,1,failed),"failed Signal retains the exact readback owner");
    expect(!failed,"retained readback owner left the caller scope");
    expect(FakeReadbackOwner::destroyed==0,"unconfirmed readback owner was not destroyed");
    expect(model_has_unresolved_owners(),"retained readback ownership reaches the coordinator query");
    const auto retainedAfterSignal=ModelOwnership::retained();

    // H05(b): Signal succeeded but the armed completion event timed out (0 of required 1).
    FakeQueue timedOut;timedOut.execute();
    auto timeout=std::make_unique<FakeReadbackOwner>();
    expect(settle_readback_owner(true,S_OK,timeout->fence.completed,1,timeout),
        "unobserved completion retains the exact readback owner");
    expect(!timeout,"timed-out readback owner left the caller scope");
    expect(FakeReadbackOwner::destroyed==0,"timed-out readback owner was not destroyed");
    expect(ModelOwnership::retained()==retainedAfterSignal+1,"timed-out owner counted exactly once");

    // Confirmed completion keeps the current normal behaviour: the caller keeps scope
    // ownership, maps/unmaps, and the stack objects release normally.
    auto confirmed=std::make_unique<FakeReadbackOwner>();
    confirmed->fence.completed=1;
    expect(classify_readback(true,S_OK,1,1)==ReadbackCompletion::Confirmed,"exact observed completion is confirmed");
    expect(!settle_readback_owner(true,S_OK,confirmed->fence.completed,1,confirmed),
        "confirmed readback is not retained");
    expect(confirmed && FakeReadbackOwner::destroyed==0,"confirmed readback keeps caller scope ownership");
    confirmed.reset();
    expect(FakeReadbackOwner::destroyed==1,"confirmed readback releases normally");
    expect(ModelOwnership::retained()==retainedAfterSignal+1,"confirmed readback adds no custody");

    // A command list that was never submitted carries no GPU ownership to retain.
    expect(classify_readback(false,E_FAIL,0,1)==ReadbackCompletion::NotSubmitted,
        "unsubmitted readback is not a completion state");
    expect(classify_readback(true,E_FAIL,0,1)==ReadbackCompletion::Unresolved,
        "failed Signal on an executed readback is unresolved");
    expect(classify_readback(true,S_OK,0,1)==ReadbackCompletion::Unresolved,
        "timed-out completion event is unresolved");

    std::printf("PASS readback_ownership checks=%u retained=%zu CPU_only=true\n",checks,ModelOwnership::retained());
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL readback_ownership %s checks=%u CPU_only=true\n",error.what(),checks);
    return 1;
}
