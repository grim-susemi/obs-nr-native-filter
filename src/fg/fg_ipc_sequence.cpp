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
   x.format==y.format&&x.colorSpace==y.colorSpace&&x.sourceMonitor==y.sourceMonitor&&x.displayCapture==y.displayCapture&&
   x.splitCompare==y.splitCompare&&x.guides==y.guides){
   step.previous=previous_;
   // Both colour identities must agree on the same NrOwned+Valid analysis pair,
   // and each guide endpoint must be keyed to the matching colour slot: a
   // self-consistent guide pair for other displayed frames is still withheld.
   const bool currentMv=b.flow.hasSharedMv&&b.flow.owner==FlowOwner::NrOwned&&b.flow.provenance==FlowProvenance::Valid&&
    b.flow.pairCurrTick&&b.flow.pairPrevTick;
   const bool previousMv=a.flow.hasSharedMv&&a.flow.owner==FlowOwner::NrOwned&&a.flow.provenance==FlowProvenance::Valid&&
    a.flow.pairCurrTick&&a.flow.pairPrevTick;
   const bool keyed=b.key!=a.key&&b.flow.currColor==b.key&&a.flow.currColor==a.key&&
    b.flow.prevColor==a.key;
   step.hasValidSharedMv=currentMv&&previousMv&&keyed&&a.flow.pairCurrTick==b.flow.pairPrevTick&&
    a.flow.pairCurrTick!=b.flow.pairCurrTick;
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
