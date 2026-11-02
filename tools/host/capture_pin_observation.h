// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "interop.h"

namespace nr::qa {
// Original provenance is collected once on graphics; liveness is checked later.
// Private construction prevents a controller from manufacturing an observation.
class CapturePinObservation final {
    const JobKey sampleKey_;
    const interop::Consumer original_;
    const DWORD observedThread_;
    CapturePinObservation(const JobKey& key,const interop::Consumer& pin,DWORD thread):
        sampleKey_(key),original_(pin),observedThread_(thread) {}
public:
    static std::optional<CapturePinObservation> observe(const JobKey& key,
        const interop::Ticket& ticket,const interop::Consumer& pin,
        const interop::Bridge& bridge,DWORD graphicsOwner,const interop::Ownership& physical) {
        if(!graphicsOwner || GetCurrentThreadId()!=graphicsOwner || !key.valid() ||
            ticket.key!=key || pin.ticket!=ticket || !pin.raw ||
            pin.graphicsOwner!=graphicsOwner || !bridge.owns_consumer(pin) ||
            !physical.key || *physical.key!=key || !physical.consumerOpen ||
            physical.failed || !pin.leaseValue || physical.consumerLease!=pin.leaseValue ||
            interop::reusable(physical)) return std::nullopt;
        return CapturePinObservation(key,pin,GetCurrentThreadId());
    }
    bool retains(const JobKey& key,const interop::Ticket& ticket,
        const interop::Consumer& pin,const interop::Bridge& bridge,
        DWORD graphicsOwner,const interop::Ownership& physical) const {
        return key==sampleKey_ && ticket.key==key && ticket==original_.ticket &&
            pin.ticket==ticket && graphicsOwner && observedThread_==graphicsOwner &&
            original_.graphicsOwner==graphicsOwner && pin.graphicsOwner==graphicsOwner &&
            original_.bridge==&bridge && pin.bridge==original_.bridge &&
            original_.fence.Get()==bridge.consumer_fence() && pin.fence.Get()==original_.fence.Get() &&
            original_.texture && pin.texture==original_.texture && original_.raw && pin.raw==original_.raw &&
            original_.leaseValue && pin.leaseValue==original_.leaseValue &&
            physical.key && *physical.key==key && physical.consumerOpen && !physical.failed &&
            physical.consumerLease==original_.leaseValue && !interop::reusable(physical);
    }
};
} // namespace nr::qa
