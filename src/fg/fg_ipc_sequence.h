// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_ipc.h"

namespace nr::fg::ipc::detail {
class CopySequence final {
 std::optional<CopyTicket> previous_;
 uint64_t generation_=0, high_=0;
public:
 using Slots=std::array<std::optional<Frame>,slot_count>;
 static std::optional<CopyTicket> oldest(const Slots&);
 const std::optional<CopyTicket>& previous()const{return previous_;}
 void break_pair(){previous_.reset();}
 CopyStep record(const CopyTicket&,Status);
};

Status copy_completion(uint64_t completed,uint64_t serial);
} // namespace nr::fg::ipc::detail
