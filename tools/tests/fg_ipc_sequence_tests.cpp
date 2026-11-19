// SPDX-License-Identifier: GPL-3.0-only
#include "fg/fg_ipc_sequence.h"
#include <cstdio>

using namespace nr::fg::ipc;
using nr::fg::ipc::detail::CopySequence;
using nr::fg::ipc::detail::copy_completion;

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
}

int main(){
 try{
  ordered_adjacent();std::puts("PASS ordered_adjacent metadata=preserved");
  discontinuities();std::puts("PASS discontinuities serial_gap generation timestamp shape");
  rejected_and_stale();std::puts("PASS rejected_and_stale pending invalid protocol peer_dead");
  fence_values();std::puts("PASS fence_values prior exact later device_lost");
  return 0;
 }catch(const std::exception& error){std::fprintf(stderr,"FAIL fg_ipc_sequence: %s\n",error.what());return 1;}
}
