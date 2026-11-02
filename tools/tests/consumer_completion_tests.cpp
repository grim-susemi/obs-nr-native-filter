// SPDX-License-Identifier: GPL-3.0-only
#include "consumer_completion_cpu.h"
#include <chrono>
#include <type_traits>
using namespace nr;
using namespace nr::interop;
static_assert(!std::is_copy_assignable_v<Consumer>);
static_assert(!std::is_copy_assignable_v<ConsumerEnd>);
namespace {
const FrameDescriptor descriptor{{16,16},{0,0,16,16},PixelFormat::Rgba8,ColorSpace::Rec709Srgb,AlphaMode::Straight,80};
struct Fixture {
 std::shared_ptr<CpuConsumerRig> rig=std::make_shared<CpuConsumerRig>();
 std::shared_ptr<Bridge> bridge;
 explicit Fixture(uint64_t signals=0,uint64_t leases=0,bool create=true){
  rig->signalSeed=signals;rig->leaseSeed=leases;rig->create=create;nextConsumerRig=rig;
  bridge=std::make_shared<Bridge>(nullptr,1,descriptor,std::span<const std::byte>{},std::span<const std::byte>{});
 }
 Ticket install(size_t slot,uint64_t sequence=1){
  const JobKey key{{41,1,sequence},1,1,sequence};
  const auto ticket=bridge->capture_at(slot,key,nullptr);expect_consumer(ticket.has_value(),"CPU capture exact ticket");
  rig->slots[slot].worker=1;rig->slots[slot].phase=WorkPhase::Submitted;return *ticket;
 }
};
void await_consumer(const std::shared_future<void>& event){
 expect_consumer(event.wait_for(std::chrono::seconds(5))==std::future_status::ready,"exact subscribed CPU event timeout");event.get();
}
struct CpuWorker final:pipeline::Processor {
 pipeline::Processed process(const pipeline::Work& work,std::stop_token)override{
  auto rig=std::static_pointer_cast<CpuConsumerRig>(work.binding.processorLease);
  rig->slots[work.binding.ticket.slot].worker=1;rig->slots[work.binding.ticket.slot].phase=WorkPhase::Submitted;
  return {work.job,true,work.raw->image};
 }
};
IngressSample input(uint64_t sequence){
 return {{41,1,sequence},sequence,sequence-1,301,901,1,SourceValidity::Valid,descriptor,
  {{1},true,"C:/cpu/not-loaded.dll",{.75f},{0,0,1,1,0,-1,true}}};
}
void production_pipeline_trace(){
 Fixture f;auto worker=std::make_shared<CpuWorker>();pipeline::FramePipeline p({41,1,1,1},worker);
 expect_consumer(p.hold_generation(1,f.rig),"actual feature generation lease");
 std::vector<Consumer> pins;std::vector<Ticket> tickets;
 for(uint64_t i=1;i<=2;++i){
  const auto sample=input(i);const auto plan=p.begin(sample,i+1);expect_consumer(plan.admission.has_value(),"actual pipeline admission");
  const auto ticket=f.bridge->capture_at(plan.admission->slot,plan.admission->job.key,nullptr);
  expect_consumer(ticket.has_value(),"exact physical capture");
  const auto pin=f.bridge->acquire_raw(*ticket);expect_consumer(pin.has_value(),"actual acquisition");
  const auto raw=std::make_shared<const pipeline::RawFrame>(pipeline::RawFrame{sample,std::make_shared<const uint64_t>(i)});
  const pipeline::WorkBinding binding{*ticket,f.bridge->ownership(ticket->slot),f.bridge,f.rig,pin};
  if(i==1){
   auto wrong=binding.captured;wrong.consumerLease+=1;
   const pipeline::WorkBinding malformed{*ticket,wrong,f.bridge,f.rig,pin};
   expect_consumer(!p.finish(raw,&malformed),"malformed captured pin identity refused");
  }
  const auto returned=plan.admission->returned; // Subscription precedes dispatch.
  expect_consumer(p.finish(raw,&binding),"actual pin validation and dispatch");await_consumer(returned);
  expect_consumer(p.observe(*ticket,f.bridge->ownership(ticket->slot)),"actual worker ownership observation");
  pins.push_back(*pin);tickets.push_back(*ticket);
 }
 const auto originalSerial=pins[0].leaseValue;
 const auto laterEvent=ConsumerEvent::create();const auto later=f.bridge->end_consumer(pins[1],laterEvent);
 expect_consumer(later&&later->completionValue==1,"later acquisition ends first with target one");
 f.rig->graphics.acknowledge_prefix(f.rig->graphics.commands.size());
 expect_consumer(p.observe(tickets[1],f.bridge->ownership(1)),"actual later completion observer");
 expect_consumer(f.bridge->ownership(0).consumerOpen&&!reusable(f.bridge->ownership(0))&&!f.bridge->close(),
  "higher original completion never closes older open lease");
 auto fake=f.bridge->ownership(0);fake.consumerOpen=false;
 expect_consumer(!p.observe(tickets[0],fake),"shared completed value is not a close receipt");
 auto malformed=f.bridge->ownership(0);malformed.consumerLease=UINT64_MAX;
 expect_consumer(!p.observe(tickets[0],malformed),"malformed failed-sentinel lease receipt");
 CpuConsumerTexture target;
 expect_consumer(f.bridge->copy_to(pins[0],target.Get()),"old last copy after later acknowledged marker");
 const auto copyPrefix=f.rig->graphics.commands.size();
 const auto fresh=ConsumerEvent::create();const auto older=f.bridge->end_consumer(pins[0],fresh);
 expect_consumer(older&&older->completionValue==2&&older->consumer.leaseValue==originalSerial&&
  older->consumer.fence.Get()==f.rig->consumer11.Get()&&older->event==fresh,"immutable exact ending receipt");
 auto overstated=f.bridge->ownership(0);overstated.consumerDone=older->completionValue;
 expect_consumer(!p.observe(tickets[0],overstated),"actual pipeline rejects invented marker completion");
 expect_consumer(p.observe(tickets[0],f.bridge->ownership(0)),"actual pending final ending observation");
 const auto exited=p.worker_exit();p.request_stop();await_consumer(exited);
 expect_consumer(!reusable(f.bridge->ownership(0))&&!f.bridge->close()&&!p.try_close()&&
  WaitForSingleObject(fresh->handle(),0)==WAIT_TIMEOUT,"pending last copy and signal cannot release");
 f.rig->graphics.acknowledge_prefix(copyPrefix);
 expect_consumer(!reusable(f.bridge->ownership(0))&&!f.bridge->close()&&!p.try_close()&&
  WaitForSingleObject(fresh->handle(),0)==WAIT_TIMEOUT,"copy acknowledgement alone cannot prove marker completion");
 f.rig->graphics.acknowledge_prefix(copyPrefix+1);
 expect_consumer(WaitForSingleObject(fresh->handle(),0)==WAIT_OBJECT_0&&
  p.observe(tickets[0],f.bridge->ownership(0))&&reusable(f.bridge->ownership(0)),"exact new signal marker completion");
 expect_consumer(!p.try_close(),"actual feature generation still retained");
 expect_consumer(p.complete_generation(1)&&p.try_close()&&f.bridge->close()&&f.bridge->closed(),"actual pipeline and Bridge closure");
 expect_consumer(!f.bridge->consumer_fence()&&completion(older->completionValue,older->consumer.fence->GetCompletedValue())==
  Completion::Complete,"genuine original close preserves receipt fence");
 std::printf("TRACE_PASS actual_pipeline=true old_lease=%llu final_target=%llu last_copy_prefix=%zu signal_prefix=%zu\n",
  originalSerial,older->completionValue,copyPrefix,copyPrefix+1);
}
void negative_controls(){
 unsigned controls=0;
 {
  Fixture f;const auto t=f.install(0);const auto old=f.bridge->acquire_raw(t);
  const auto ended=f.bridge->end_consumer(*old);f.rig->graphics.acknowledge_prefix(1);
  const auto fresh=f.bridge->acquire_raw(t);CpuConsumerTexture target;
  expect_consumer(ended&&fresh&&old->ticket==fresh->ticket&&old->leaseValue!=fresh->leaseValue&&
   !f.bridge->copy_to(*old,target.Get())&&!f.bridge->end_consumer(*old)&&!f.bridge->close(),"stale same-ticket reacquisition");
  ++controls;
 }
 for(unsigned field=0;field<6;++field){
  Fixture f;const auto lease=f.bridge->acquire_raw(f.install(0));const auto& k=lease->ticket.key;
  const JobKey wrong{{k.frame.instanceId+(field==0),k.frame.contentEpoch+(field==1),k.frame.ingressSequence+(field==2)},
   k.historyEpoch+(field==3),k.resourceGeneration+(field==4),k.submissionSerial+(field==5)};
  const Consumer forged{{wrong,0},lease->texture,lease->leaseValue,lease->raw,lease->bridge,lease->fence,lease->graphicsOwner};
  CpuConsumerTexture target;
  expect_consumer(!f.bridge->owns_consumer(forged)&&!f.bridge->copy_to(forged,target.Get())&&!f.bridge->end_consumer(forged)&&
   f.bridge->ownership(0).consumerOpen&&!f.bridge->close(),"full immutable key refusal");++controls;
 }
 for(unsigned field=0;field<7;++field){
  Fixture f,other;const auto lease=f.bridge->acquire_raw(f.install(0));CpuConsumerTexture texture;
  const Consumer forged{{lease->ticket.key,field==0?size_t{1}:size_t{0}},field==5?texture.Get():lease->texture,
   lease->leaseValue+(field==1),field==6?!lease->raw:lease->raw,field==2?other.bridge.get():lease->bridge,
   field==3?CpuCom<ID3D11Fence>{other.rig->consumer11}:lease->fence,field==4?DWORD{0}:lease->graphicsOwner};
  expect_consumer(!f.bridge->owns_consumer(forged)&&!f.bridge->end_consumer(forged)&&
   f.rig->graphics.commands.empty()&&!f.bridge->close(),"original slot serial Bridge fence owner texture raw refusal");++controls;
 }
 {
  Fixture f;const auto lease=f.bridge->acquire_raw(f.install(0));bool refused=false;
  std::thread caller([&]{CpuConsumerTexture target;refused=!f.bridge->copy_to(*lease,target.Get())&&!f.bridge->end_consumer(*lease);});
  caller.join();expect_consumer(refused&&f.bridge->ownership(0).consumerOpen&&!f.bridge->close(),"actual wrong graphics thread");++controls;
 }
 {
  Fixture f;const auto old=f.bridge->acquire_raw(f.install(0));const auto later=f.bridge->acquire_raw(f.install(1,2));
  auto oldEvent=ConsumerEvent::create();expect_consumer(SUCCEEDED(f.rig->consumer11->SetEventOnCompletion(old->leaseValue,oldEvent->handle())),"old acquisition event fixture");
  expect_consumer(static_cast<bool>(f.bridge->end_consumer(*later)),"later marker");
  f.rig->graphics.acknowledge_prefix(1);
  expect_consumer(!f.bridge->end_consumer(*old,oldEvent)&&f.bridge->ownership(0).consumerOpen&&!f.bridge->close(),"old signalled event is not ending receipt");++controls;
 }
 {
  Fixture f;const auto old=f.bridge->acquire_raw(f.install(0));auto event=ConsumerEvent::create();
  const auto ended=f.bridge->end_consumer(*old,event);
  const auto fresh=f.bridge->acquire_raw(old->ticket);
  expect_consumer(ended&&fresh&&!f.bridge->end_consumer(*fresh,event)&&f.bridge->ownership(0).consumerOpen,"one-use bound event even while pending");++controls;
 }
 for(unsigned policy=0;policy<5;++policy){
  Fixture f(policy==1?UINT64_MAX-1:policy==2?UINT64_MAX:0);const auto lease=f.bridge->acquire_raw(f.install(0));
  const auto event=ConsumerEvent::create();
  if(policy==0)f.rig->consumer11->done=UINT64_MAX;
  if(policy==3)f.rig->consumer11->bindingResult=E_INVALIDARG;
  if(policy==4)f.rig->graphics.signalResult=E_INVALIDARG;
  expect_consumer(!f.bridge->end_consumer(*lease,event)&&f.bridge->ownership(0).consumerOpen&&!f.bridge->close(),"MAX exhaustion bind or Signal retains ownership");
  expect_consumer(f.bridge->ownership(0).failed==(policy>=3),"sticky failure truthful");
  if(policy>=3)expect_consumer(f.bridge->ownership(0).consumer==1&&f.rig->slots[0].endEvent==event&&
   !f.bridge->end_consumer(*lease,ConsumerEvent::create()),"attempted exact target event retained through sticky failure");
  ++controls;
 }
 {
  Fixture f(0,UINT64_MAX-1);const auto t=f.install(0);
  expect_consumer(!f.bridge->acquire_raw(t)&&f.bridge->ownership(0).key.has_value(),"lease cursor exhaustion preserves unknown owner");++controls;
 }
 {
  Fixture f;const auto t=f.install(0);expect_consumer(f.bridge->cancel(t),"cancellation fixture");
  expect_consumer(!f.bridge->acquire_output(t)&&!publishable(f.bridge->ownership(0),t.key),"cancelled work cannot publish");
  const auto raw=f.bridge->acquire_raw(t);expect_consumer(raw.has_value(),"cancelled raw remains drainable");
  expect_consumer(static_cast<bool>(f.bridge->end_consumer(*raw))&&!f.bridge->close(),"cancelled drain still needs exact physical completion");
  f.rig->graphics.acknowledge_prefix(1);expect_consumer(f.bridge->close(),"cancelled drain only closes after marker");++controls;
 }
 {
  Fixture f;const auto t=f.install(0);f.rig->slots[0].failed=true;
  expect_consumer(!f.bridge->acquire_raw(t)&&!f.bridge->acquire_output(t)&&!f.bridge->close(),"preexisting failure remains retained");++controls;
 }
 {
  Fixture f;f.install(0);f.rig->slots[0].worker=0;f.rig->slots[0].phase=WorkPhase::Captured;
  expect_consumer(!f.bridge->close()&&!reusable(f.bridge->ownership(3)),"unknown unsubmitted ownership and invalid slot cannot close");++controls;
 }
 {
  Fixture f(0,0,false);expect_consumer(!f.bridge->closed()&&!f.bridge->close(),"missing Impl is not accepted original close");++controls;
 }
 for(unsigned pending=0;pending<2;++pending){
  Fixture f;const auto lease=f.bridge->acquire_raw(f.install(0));
  expect_consumer(static_cast<bool>(f.bridge->end_consumer(*lease)),"pending dependency fixture");
  f.rig->graphics.acknowledge_prefix(1);
  if(pending==0)f.rig->worker12->done=0;else f.rig->producer11->done=0;
  expect_consumer(!reusable(f.bridge->ownership(0))&&!f.bridge->close(),"pending producer worker dependency retained");++controls;
 }
 std::printf("NEGATIVES_PASS controls=%u actual_transition_extraction=true\n",controls);
}
}
int main(){
 try{production_pipeline_trace();negative_controls();std::printf("PASS consumer_completion checks=%u CPU_ONLY=true\n",consumerChecks);return 0;}
 catch(const std::exception& e){std::fprintf(stderr,"FAIL consumer_completion: %s\n",e.what());return 1;}
}
