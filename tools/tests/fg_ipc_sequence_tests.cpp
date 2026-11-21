// SPDX-License-Identifier: GPL-3.0-only
#include "fg/fg_ipc_sequence.h"
#include "../fg-presenter/vfg_cadence.h"
#include "../fg-presenter/vfg_content.h"
#include <cstdio>

using namespace nr::fg::ipc;
using nr::fg::ipc::detail::CopySequence;
using nr::fg::ipc::detail::copy_completion;
using namespace nr::fg::vfg;

namespace {
Frame original(uint64_t serial,uint64_t timestamp,uint64_t generation=7){
 Frame f{generation,serial,timestamp,timestamp+9,{1920,1080,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709}};
 return f;
}
void expect(bool result,const char* message){if(!result)throw std::runtime_error(message);}

std::optional<Frame> read_wire(uint64_t serial,uint64_t timestamp){
 WireFrame wire{};
 const Frame input=original(serial,timestamp);
 InterlockedExchange64(&wire.sequence,static_cast<LONG64>(serial*2-1));
 InterlockedExchange64(&wire.words[0],static_cast<LONG64>(input.generation));
 InterlockedExchange64(&wire.words[1],static_cast<LONG64>(input.serial));
 InterlockedExchange64(&wire.words[2],static_cast<LONG64>(input.timestampNs));
 InterlockedExchange64(&wire.words[3],static_cast<LONG64>(input.arrivalNs));
 InterlockedExchange64(&wire.words[4],(LONG64{1080}<<32)|1920);
 InterlockedExchange64(&wire.words[5],static_cast<LONG64>(input.shape.format));
 InterlockedExchange64(&wire.sequence,static_cast<LONG64>(serial*2));
 return snapshot(wire);
}

void ordered_adjacent(){
 // Given committed wire records in slot order rather than time order,
 // parse, select and pair using the production IPC code.
 CopySequence::Slots slots{};
 expect(!CopySequence::oldest(slots),"idle ring has no copy candidate");
 slots[0]=read_wire(42,1040);slots[3]=read_wire(41,1000);
 CopySequence sequence;
 const auto a=CopySequence::oldest(slots);
 expect(a&&a->slot==3,"lowest serial first, not lowest slot");
 expect(!sequence.record(*a,Status::Ok).previous,"first input is original only");
 slots[3].reset();
 const auto b=CopySequence::oldest(slots);
 expect(b&&b->slot==0,"second visible slot selected");
 const auto pair=sequence.record(*b,Status::Ok);
 expect(pair.current&&pair.previous&&pair.previous->slot==3&&pair.previous->frame.serial==41&&
  pair.current->frame.serial==42&&pair.current->frame.timestampNs==1040&&pair.current->frame.arrivalNs==1049&&
  pair.current->frame.shape.width==1920,"pair preserves both owners and real metadata");
}

void discontinuities(){
 // Given an earlier original, a gap, new generation, regressed time or changed shape must reseed.
 const CopyTicket a{0,original(1,100)};
 auto changed=original(2,200);changed.shape.colorSpace=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
 const std::array cases{original(3,300),original(2,200,8),original(2,99),changed};
 for(const auto& input:cases){
  CopySequence sequence;
  sequence.record(a,Status::Ok);
  auto step=sequence.record({1,input},Status::Ok);
  expect(step.current&&!step.previous,"discontinuity exposes only the current original");
  auto next=input;++next.serial;++next.timestampNs;
  auto resumed=sequence.record({2,next},Status::Ok);
  expect(resumed.previous&&resumed.previous->frame.serial==input.serial,"new chain starts at the discontinuity");
 }
}

void rejected_and_stale(){
 // Given a good input, a rejected copy reports no ticket and breaks the chain even on retry.
 for(const auto failure:{Status::Pending,Status::InvalidFrame,Status::ProtocolError,Status::PeerDead}){
  CopySequence sequence;
  sequence.record({0,original(1,100)},Status::Ok);
  const CopyTicket b{1,original(2,200)};
  auto rejected=sequence.record(b,failure);
  expect(rejected.status==failure&&!rejected.current&&!rejected.previous,"failed submit emits no ticket");
  expect(!sequence.record(b,Status::Ok).previous,"retry cannot pair across a failed submission");
  const auto stale=sequence.record({2,original(1,100)},Status::Ok);
  expect(stale.status==Status::Ok&&stale.current&&!stale.previous,"late slot can drain without pairing");
  auto resumed=sequence.record({3,original(3,300)},Status::Ok);
  expect(resumed.previous&&resumed.previous->frame.serial==2,"late slot does not rewind the pair chain");
 }
}

void fence_values(){
 // Given a submitted serial, a pending fence or removed device never permits reuse.
 expect(copy_completion(41,42)==Status::Pending,"earlier return is not completion");
 expect(copy_completion(42,42)==Status::Ok,"exact return is completion");
 expect(copy_completion(55,42)==Status::Ok,"later value on same fence preserves prior completion");
 expect(copy_completion(UINT64_MAX,42)==Status::DeviceLost,"removed-device sentinel is not completion");
}

void retained_unique(){
 ContentHistory history;
 CopySequence source;
 std::array<char,content_input_count> textures{};
 const auto admit=[&](const Frame& frame,char pixels){
  const auto index=history.input_index();
  const auto old=history.anchor();
  expect(!old||index!=old->index,"the next copy cannot overwrite the retained unique original");
  textures[index]=pixels;
  const auto step=source.record({0,frame},Status::Ok);
  return history.observe(index,frame,step.previous.has_value());
 };
 const auto a=original(1,1'000'000'000);
 const auto anchorIndex=history.input_index();
 expect(!admit(a,'A').continuous&&!history.can_compare(),"first original reseeds");
 history.retain(anchorIndex,a);
 const auto dupIndex=history.input_index();
 expect(admit(original(2,a.timestampNs+period_60hz),'A').continuous,"duplicate can continue the transport");
 expect(history.can_compare()&&textures[dupIndex]==textures[anchorIndex],"compare to original pixels");
 expect(history.anchor()->frame.serial==1,"dedup keeps last unique metadata");
 const auto nextIndex=history.input_index();
 expect(nextIndex!=dupIndex,"the next copy cannot overwrite CopySequence's predecessor either");
 const auto b=original(3,a.timestampNs+period_30hz);
 expect(admit(b,'B').continuous&&textures[anchorIndex]=='A'&&textures[nextIndex]=='B',
  "A,A,B forms a real A-B pair across transport serials 1 and 3");
 history.retain(nextIndex,b);
 const auto stale=admit(original(2,a.timestampNs+period_60hz),'Z');
 expect(stale.stale&&history.anchor()->frame.serial==3,"stale input cannot replace the last unique original");
 expect(admit(original(4,b.timestampNs+period_60hz),'B').continuous,"stale input cannot rewind transport history");
}

void content_boundaries(){
 const auto a=original(10,1'000'000'000);
 auto shapeChanged=original(11,a.timestampNs+period_60hz);
 shapeChanged.shape.colorSpace=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
 const std::array boundaries{
  original(12,a.timestampNs+period_60hz),  // shot change or missing serial
  original(11,a.timestampNs+period_60hz,8), // new generation
  original(11,a.timestampNs),              // nonmonotonic clock
  shapeChanged
 };
 for(const auto& b:boundaries){
  ContentHistory history;
  const auto index=history.input_index();
  (void)history.observe(index,a,false);
  history.retain(index,a);
  const auto next=history.input_index();
  expect(!history.observe(next,b,true).continuous&&!history.can_compare(),
   "no comparison across a serial, clock, shape or generation discontinuity");
  expect(history.input_index()!=next,"discontinuity still reserves the last IPC destination");
 }
 ContentHistory history;
 const auto index=history.input_index();
 (void)history.observe(index,a,false);
 history.retain(index,a);
 const auto next=history.input_index();
 expect(!history.observe(next,original(11,a.timestampNs+period_60hz),false).continuous&&!history.can_compare(),
  "transport must certify adjacency even when the serial is consecutive");
 history.retain(next,original(11,a.timestampNs+period_60hz));
 history.invalidate();
 expect(!history.can_compare()&&history.input_index()!=next,"late unique invalidates pair without releasing IPC reservation");
}

void cadence_rates(){
 for(const auto expected:{period_30hz,period_40hz,period_60hz}){
  UniqueCadence cadence;
  uint64_t now=1'000'000'000;
  expect(cadence.observe(now)==0,"one original does not establish cadence");
  for(unsigned i=0;i<12;++i){
   // At 40 Hz sampled by OBS at 60 Hz, one and two ticks alternate.
   now+=expected==period_40hz?(i%2==0?period_60hz:period_30hz):expected;
   expect(cadence.observe(now)==(i<3?0:expected),"30, 40 and 60 Hz acquisition");
  }
  expect(cadence.observe(now+100'000'000)==0,"a stall reseeds without pairing stale content");
  cadence.reset();
  expect(cadence.observe(now)==0,"session boundary clears cadence");
 }
 UniqueCadence unsupported;
 uint64_t now=1'000'000'000;
 for(unsigned i=0;i<10;++i,now+=20'500'000)
  expect(unsupported.observe(now)==0,"unsupported cadence is not folded into a target");
 expect(provisional_60hz(now,now+period_60hz,false),
  "confirmed 60 Hz content can interpolate before the estimator locks");
 expect(!provisional_60hz(now,now+period_60hz,true) &&
  !provisional_60hz(now,now+period_30hz,false),
  "a repeated input or a slow pair must acquire cadence normally");
}

void cadence_schedule(){
 for(const auto period:{period_30hz,period_40hz,period_60hz}){
  for(uint32_t mult=2;mult<=4;++mult){
   for(unsigned phase=0;phase<2;++phase){
    uint64_t arrival=1'000'000'000;
    uint64_t previous=0;
    for(unsigned i=0;i<12;++i){
     const auto plan=plan_cadence(arrival,previous,period,mult);
     expect(plan.target(1)>=arrival&&plan.target(mult)==plan.originalNs,
      "synthetics start at or after arrival and the last slot is the original");
     if(previous) expect(plan.originalNs-previous==period,"pacing phase survives both sampled 40 Hz rhythms");
     for(uint32_t slot=2;slot<=mult;++slot){
      const auto spacing=plan.target(slot)-plan.target(slot-1);
      expect(spacing==period/mult||spacing==period/mult+1,"even synthetic spacing");
     }
     previous=plan.originalNs;
     arrival+=period==period_40hz?((i+phase)%2==0?period_60hz:period_30hz):period;
    }
    expect(plan_cadence(arrival+90'000'000,previous,period,mult).target(1)>arrival,
     "after a stall the schedule moves forward");
   }
  }
 }
 expect(!plan_cadence(1'000'000'000,0,0,4).target(1),"unacquired cadence creates no synthetic slot");
 const auto tight=plan_cadence(1'000'000'000,0,period_60hz,4);
 for(uint32_t slot=1;slot<4;++slot){
  expect(tight.synthetic_cutoff(slot)>tight.target(slot)+3'500'000,
   "a subframe ready 3.5 ms after its 240 Hz target still fits");
  expect(tight.synthetic_cutoff(slot)+250'000==tight.target(slot+1),
   "each 240 Hz cutoff reserves time for the next slot, including the original");
 }
 expect(tight.synthetic_cutoff(0)==0&&tight.synthetic_cutoff(4)==0,
  "only synthetic slots have a readiness cutoff");
 const auto normal=plan_cadence(1'000'000'000,0,period_60hz,3);
 expect(normal.synthetic_cutoff(1)==normal.target(1)+3'000'000,
  "lower presentation rates keep their late tolerance");
}
}

int main(){
 try{
  ordered_adjacent();std::puts("PASS ordered_adjacent metadata=preserved");
  discontinuities();std::puts("PASS discontinuities serial_gap generation timestamp shape");
  rejected_and_stale();std::puts("PASS rejected_and_stale pending invalid protocol peer_dead");
  fence_values();std::puts("PASS fence_values prior exact later device_lost");
  retained_unique();std::puts("PASS retained_unique duplicate transport gap and stale copy");
  content_boundaries();std::puts("PASS content_boundaries cuts generations clocks shape and invalidation");
  cadence_rates();std::puts("PASS cadence_rates 30/40/60 acquisition, stall and unsupported rate");
  cadence_schedule();std::puts("PASS cadence_schedule 2x/3x/4x intervals and both 40 Hz phases");
  return 0;
 }catch(const std::exception& error){std::fprintf(stderr,"FAIL fg_ipc_sequence: %s\n",error.what());return 1;}
}
