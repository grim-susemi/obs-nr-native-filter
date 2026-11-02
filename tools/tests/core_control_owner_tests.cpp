// SPDX-License-Identifier: GPL-3.0-only
#include "../host/nr_core_control_owner.h"
#include <array>
#include <cstdio>
#include <thread>
#include <future>
using namespace nr::qa;
namespace {
unsigned checks=0;
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct External {
    std::array<unsigned,6> calls{};
    unsigned registered=0,removed=0,destroyed=0;
    std::mutex callbackMutex;
    HANDLE removeEntered=nullptr;
    bool bridgeComplete=false,workerComplete=false,registerThrows=false,bridgeThrows=false;
    bool controlledWorker=false;
    std::shared_future<void> workerCompletion;
    void* callback=nullptr;void(*dispatch)(void*)=nullptr;
    nr::lifecycle::ModelClose receipt;
};
class Backend final : public CoreControlBackend {
    External& e;
public:
    explicit Backend(External& value):e(value) {}
    ~Backend() override {++e.destroyed;}
    void register_callback(void* value,void(*fn)(void*)) override {
        ++e.registered;e.callback=value;e.dispatch=fn;
        if(e.registerThrows)throw std::runtime_error("uncertain callback registration");
    }
    void remove_callback() override {
        if(e.removeEntered)SetEvent(e.removeEntered);
        std::lock_guard lock(e.callbackMutex);
        ++e.removed;e.callback=nullptr;e.dispatch=nullptr;
    }
    CoreCloseOutcome close_step(CoreCloseStep step) override {
        ++e.calls[static_cast<size_t>(step)];
        if(step==CoreCloseStep::Worker){
            if(e.controlledWorker && (e.workerCompletion.wait_for(std::chrono::seconds(0))!=std::future_status::ready || !e.receipt.coherent))return CoreCloseOutcome::Pending;
            return e.workerComplete?CoreCloseOutcome::Complete:CoreCloseOutcome::Refused;
        }
        if(step==CoreCloseStep::Pipeline && e.calls[3]>1)throw std::runtime_error("pipeline already reset");
        if(step==CoreCloseStep::Bridge){if(e.bridgeThrows){e.bridgeThrows=false;throw std::runtime_error("Bridge close exception");}return e.bridgeComplete?CoreCloseOutcome::Complete:CoreCloseOutcome::Refused;}
        return CoreCloseOutcome::Complete;
    }
    nr::lifecycle::ModelClose diagnostics() const override {return e.receipt;}
};
void identity(){
 FILETIME creation{},exit{},kernel{},user{};wchar_t image[32768]{};
 if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user) || !GetModuleFileNameW(nullptr,image,32768))throw std::runtime_error("CPU identity");
 std::printf("CPU_PROCESS pid=%lu creation_filetime=%llu image=%ls\n",GetCurrentProcessId(),(uint64_t{creation.dwHighDateTime}<<32)|creation.dwLowDateTime,image);
}
}
int main()try{
 identity();External e;
 nr::lifecycle::VendorTrace trace;trace.bind(7,8,9,10);
 e.receipt.vendor=trace.snapshot();
 {
 auto owner=std::make_unique<CoreControlOwner>(std::make_unique<Backend>(e));owner->attach();
 auto retained=*owner;
 expect(e.registered==1 && e.removed==0,"owned callback registered");
 expect(owner->request(1),"first request admitted");
 expect(!owner->wait(0),"timeout does not manufacture response");
 expect(!owner->request(2) && owner->snapshot().pendingRequest==1,"pending request cannot be overwritten");
 e.dispatch(e.callback);
 expect(owner->wait(0) && !owner->snapshot().closed,"worker refusal returned to coordinator");
 expect(e.removed==0 && e.destroyed==0,"refused owner remains reachable");
 e.workerComplete=true;
 expect(owner->request(2),"fresh request after refusal");
 e.dispatch(e.callback);
 expect(!owner->snapshot().closed && owner->snapshot().completedSteps==4,"partial pipeline then Bridge refusal");
 expect(owner->request(3),"next request after partial close");
 e.dispatch(e.callback);
 expect(!owner->snapshot().exception && e.calls[3]==1,"partially closed pipeline not repeated on retry");
 e.bridgeComplete=true;
 expect(owner->request(4),"cooperative completion request");
 owner.reset();
 expect(e.destroyed==0 && e.removed==0,"owner facade destruction preserves whole pending callback/backend");
 e.dispatch(e.callback);
 expect(retained.wait(0) && retained.snapshot().closed,"real controlled completion");
 expect(retained.detach() && e.removed==1,"callback removed only after normal close");
 expect(retained.detach() && e.removed==1,"detach idempotent");
 expect(retained.snapshot().model.vendor.owner==7,"cached owner trace survives partial close");
 }
 expect(e.destroyed==1 && e.removed==1,"exactly one destruction after callback removal");
 External inflight;inflight.workerComplete=true;inflight.bridgeComplete=true;
 HANDLE replied=CreateEventW(nullptr,TRUE,FALSE,nullptr),release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 inflight.removeEntered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 {
 CoreControlOwner owner(std::make_unique<Backend>(inflight));owner.attach();
 expect(owner.request(1),"inflight request admitted");
 std::thread callback([&]{std::lock_guard lock(inflight.callbackMutex);inflight.dispatch(inflight.callback);SetEvent(replied);WaitForSingleObject(release,15000);});
 const bool reply=WaitForSingleObject(replied,15000)==WAIT_OBJECT_0 && owner.wait(0);
 std::thread coordinator([&]{(void)owner.detach();});
 const bool removalEntered=WaitForSingleObject(inflight.removeEntered,15000)==WAIT_OBJECT_0;
 const bool retainedBeforeReturn=inflight.removed==0 && inflight.destroyed==0;
 SetEvent(release);callback.join();coordinator.join();
 expect(reply && removalEntered && retainedBeforeReturn,"reply does not prove callback return or allow premature destruction");
 expect(inflight.removed==1 && !owner.snapshot().callbackAttached,"coordinator detach serialized after callback return");
 }
 expect(inflight.destroyed==1,"inflight whole backend destroyed only after serialized detach");
 CloseHandle(replied);CloseHandle(release);CloseHandle(inflight.removeEntered);
 External uncertain;uncertain.registerThrows=true;uncertain.workerComplete=true;uncertain.bridgeComplete=true;
 {
 auto owner=std::make_unique<CoreControlOwner>(std::make_unique<Backend>(uncertain));
 bool threw=false;try{owner->attach();}catch(const std::exception&){threw=true;}
 auto retained=*owner;owner.reset();
 expect(threw && uncertain.destroyed==0 && uncertain.removed==0,"registration exception retains actual whole owner");
 uncertain.bridgeThrows=true;expect(retained.request(1),"exception close admitted");uncertain.dispatch(uncertain.callback);
 expect(retained.snapshot().exception && retained.snapshot().completedSteps==4 && !retained.snapshot().closed,"exception preserves successful partial steps");
 expect(retained.request(2),"same owner exception retry admitted");uncertain.dispatch(uncertain.callback);
 expect(retained.snapshot().closed && uncertain.calls[3]==1 && retained.detach(),"exception retry never repeats released pipeline");
 }
 expect(uncertain.destroyed==1 && uncertain.removed==1,"exception owner destroyed once after actual removal");
 External pending;pending.controlledWorker=true;pending.workerComplete=true;pending.bridgeComplete=true;pending.receipt.coherent=false;
 std::promise<void> workerDone;pending.workerCompletion=workerDone.get_future().share();
 HANDLE workerRelease=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 std::thread worker([&]{WaitForSingleObject(workerRelease,15000);workerDone.set_value();});
 {
 CoreControlOwner owner(std::make_unique<Backend>(pending));owner.attach();
 expect(owner.request(1),"controlled worker first request admitted");pending.dispatch(pending.callback);
 const auto paused=owner.snapshot();const bool signaled=owner.wait(0);const bool replaced=owner.request(2);
 std::printf("CPU_PENDING request=%llu pendingRequest=%llu pending=%s event_signaled=%s steps=%zu coherent=%s request2=%s\n",paused.request,paused.pendingRequest,paused.pending?"true":"false",signaled?"true":"false",paused.completedSteps,paused.model.coherent?"true":"false",replaced?"true":"false");
 SetEvent(workerRelease);worker.join();CloseHandle(workerRelease);
 expect(paused.pending && paused.pendingRequest==1 && paused.completedSteps==1 && !paused.model.coherent && !signaled && !replaced,"incomplete controlled worker preserves same request unsignaled");
 for(unsigned i=0;i<3;++i)pending.dispatch(pending.callback);
 expect(owner.snapshot().pending && owner.snapshot().completedSteps==1 && !owner.wait(0) && pending.calls[0]==1,"ready worker incoherent diagnostics keep request pending over callbacks");
 std::printf("CPU_READY_INCOHERENT pending=true event_signaled=false steps=1 stop_calls=%u\n",pending.calls[0]);
 pending.receipt.coherent=true;
 HANDLE returned=CreateEventW(nullptr,TRUE,FALSE,nullptr),callbackRelease=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 pending.removeEntered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 std::thread callback([&]{std::lock_guard lock(pending.callbackMutex);pending.dispatch(pending.callback);SetEvent(returned);WaitForSingleObject(callbackRelease,15000);});
 const bool complete=WaitForSingleObject(returned,15000)==WAIT_OBJECT_0 && owner.wait(0);
 const auto done=owner.snapshot();
 std::thread detach([&]{(void)owner.detach();});
 const bool removing=WaitForSingleObject(pending.removeEntered,15000)==WAIT_OBJECT_0;
 const bool stillRetained=pending.removed==0 && pending.destroyed==0;
 SetEvent(callbackRelease);callback.join();detach.join();
 expect(complete && done.request==1 && done.closed && done.completedSteps==6,"controlled same request completes six stages");
 expect(removing && stillRetained && pending.removed==1 && !owner.snapshot().callbackAttached,"same request detach serialized after actual callback return");
 expect(pending.calls[0]==1 && pending.calls[3]==1,"pending progression never replays successful stages");
 CloseHandle(returned);CloseHandle(callbackRelease);CloseHandle(pending.removeEntered);
 }
 expect(pending.destroyed==1 && pending.removed==1,"pending whole owner destroyed after callback detach");
 External refused;refused.receipt.core=nr::lifecycle::CloseResult::Refused;refused.receipt.coherent=true;refused.bridgeComplete=true;
 {
 CoreControlOwner owner(std::make_unique<Backend>(refused));owner.attach();expect(owner.request(1),"coherent refusal request admitted");refused.dispatch(refused.callback);
 const auto receipt=owner.snapshot();const auto calls=refused.calls;
 for(unsigned i=0;i<3;++i)refused.dispatch(refused.callback);
 expect(owner.wait(0) && !receipt.pending && !receipt.closed && receipt.model.coherent && receipt.model.core==nr::lifecycle::CloseResult::Refused && refused.calls==calls,"coherent refusal terminal with no render automatic retry");
 expect(!owner.detach() && refused.destroyed==0,"coherent refusal retains whole graph");
 refused.workerComplete=true;expect(owner.request(2),"coherent refusal retry only explicit request");refused.dispatch(refused.callback);
 expect(owner.snapshot().closed && refused.calls[0]==1 && owner.detach(),"explicit refusal retry continues exact successful stages");
 }
 std::printf("PASS core_control_owner checks=%u CPU_only=true\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL core_control_owner %s checks=%u CPU_only=true\n",e.what(),checks);return 1;}
