// SPDX-License-Identifier: GPL-3.0-only
#include "fg_ipc_sequence.h"

namespace nr::fg::ipc::detail {
std::optional<CopyTicket> CopySequence::oldest(const Slots& slots){
 std::optional<CopyTicket> next;
 for(size_t i=0;i<slots.size();++i){
  if(slots[i]&&(!next||slots[i]->serial<next->frame.serial))next=CopyTicket{i,*slots[i]};
 }
 return next;
}
CopyStep CopySequence::record(const CopyTicket& ticket,Status result){
 if(result!=Status::Ok){break_pair();return {result};}
 const auto& b=ticket.frame;
 if(generation_!=b.generation){generation_=b.generation;high_=0;break_pair();}
 // Still return the copy ticket for a late slot: it was submitted and must drain.
 CopyStep step{Status::Ok,ticket};
 if(b.serial<=high_)return step;
 if(previous_){
  const auto& a=previous_->frame;
  const auto& x=a.shape;const auto& y=b.shape;
  if(b.serial-a.serial==1&&b.timestampNs>a.timestampNs&&x.width==y.width&&x.height==y.height&&
   x.format==y.format&&x.colorSpace==y.colorSpace&&x.sourceMonitor==y.sourceMonitor&&x.displayCapture==y.displayCapture){
   step.previous=previous_;
  }
 }
 high_=b.serial;previous_=ticket;
 return step;
}
Status copy_completion(uint64_t completed,uint64_t serial){
 if(completed==UINT64_MAX)return Status::DeviceLost;
 return completed>=serial?Status::Ok:Status::Pending;
}
} // namespace nr::fg::ipc::detail
