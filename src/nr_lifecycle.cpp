// SPDX-License-Identifier: GPL-3.0-only
#include "nr_lifecycle.h"

namespace nr::lifecycle {
bool SnapshotRetirement::retire(const std::array<std::shared_ptr<const void>,2>& owners,uint64_t fence)
{
    if(!owners[0] && !owners[1])return true; // Null reentry preserves an earlier cutoff.
    if(!fence || fence==UINT64_MAX)return false;
    if(!capture_allowed())return owners==held_ && fence==fence_;
    held_=owners;fence_=fence;return true;
}
bool SnapshotRetirement::collect(uint64_t generation,uint64_t completed)
{
    if(generation!=generation_ || completed==UINT64_MAX)return false;
    if(!capture_allowed() && completed<fence_)return false;
    held_={};fence_=0;return true;
}
Decision Observer::prepare(const Observation& next, bool cut, Reason boundaryProblem)
{
    const auto decision = [&](std::optional<ResetCause> cause, bool reset, Reason problem) -> Decision {
        std::optional<Selection> placeholder;
        if (problem != Reason::None)
            placeholder.emplace(Selection{next.canonicalTick, OutputKind::CurrentSource,
                next.settings.enabled ? Status::Bypassed : Status::Disabled, problem, {}, {}, reset});
        return {pipeline_.epochs(), cause, reset, placeholder};
    };
    Reason problem = boundaryProblem;
    if (problem == Reason::None) {
        if (!next.canonicalTick || !next.sourceIdentity || !next.deviceIdentity) problem = Reason::MissingIdentity;
        else if (next.validity == SourceValidity::Invalid) problem = Reason::InvalidSource;
        else if (next.validity != SourceValidity::Valid) problem = Reason::UnknownValidity;
        else if (!next.descriptor.valid()) problem = Reason::InvalidDescriptor;
        else if (!next.settings.valid()) problem = Reason::InvalidSettings;
    }
    if (problem != Reason::None) {
        const bool reset = !invalid_ && !blocked_ && pipeline_.reset(ResetCause::InvalidInput);
        invalid_ = true;
        return decision(reset ? std::optional{ResetCause::InvalidInput} : std::nullopt, reset, problem);
    }

    std::optional<ResetCause> cause;
    if (configured_) {
        const auto& old = *configured_;
        // A single replacement covers simultaneous changes; both histories and
        // delayed eligibility share its immutable content/resource boundary.
        if (next.deviceIdentity != old.deviceIdentity) cause = ResetCause::DeviceChanged;
        else if (next.descriptor.allocation != old.descriptor.allocation || next.descriptor.active != old.descriptor.active)
            cause = ResetCause::ExtentChanged;
        else if (next.descriptor != old.descriptor) cause = ResetCause::FormatChanged;
        else if (next.settings.revision != old.settings.revision || next.settings.runtimePath != old.settings.runtimePath ||
                 next.settings.strength != old.settings.strength || next.settings.tuning != old.settings.tuning)
            cause = ResetCause::SettingsChanged;
        else if (next.sourceIdentity != old.sourceIdentity) cause = ResetCause::SourceChanged;
        else if (next.settings.enabled != old.settings.enabled) cause = ResetCause::Disabled;
    } else if (!next.settings.enabled) cause = ResetCause::Disabled;
    if (!cause && (invalid_ || blocked_)) cause = ResetCause::InvalidInput;
    if (!cause && cut) cause = ResetCause::Cut;

    bool reset = false;
    if (cause) {
        const auto before = pipeline_.epochs();
        reset = pipeline_.reset(*cause);
        if (!reset) {
            const auto transition = reset_epochs(before, *cause);
            if (!transition || transition->next.resourceGeneration == before.resourceGeneration)
                return decision(cause, false, Reason::KeyMismatch);
            // Accepted resource refusal cancels slots but does not advance
            // epochs/discard previous. Cut logically once, retain the desired
            // change, and do not let this reset's cleared internal budget flag
            // authorize work against old resources.
            const bool cutoff = !blocked_ && pipeline_.reset(ResetCause::InvalidInput);
            const bool alreadyCut = blocked_;
            blocked_ = cutoff || alreadyCut;
            invalid_ = false;
            return decision(cause, cutoff, cutoff || alreadyCut ? Reason::RetirementBudget : Reason::KeyMismatch);
        }
    }
    configured_.emplace(next);
    invalid_ = false;
    blocked_ = false;
    return decision(cause, reset, next.settings.enabled ? Reason::None : Reason::Cancelled);
}
} // namespace nr::lifecycle
