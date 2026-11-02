// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
namespace nr::qa {
// Exact completion state of a QA readback whose command list was submitted.
enum class ReadbackCompletion { NotSubmitted, Confirmed, Unresolved };
// Pure decision for the real readback path. A submission is confirmed only when the queue
// Signal succeeded AND the armed completion event observed the exact required fence value.
inline ReadbackCompletion classify_readback(bool executed,HRESULT signal,uint64_t observed,uint64_t required) noexcept {
    if(!executed)return ReadbackCompletion::NotSubmitted;
    return SUCCEEDED(signal) && observed==required?ReadbackCompletion::Confirmed:ReadbackCompletion::Unresolved;
}
// Process-lifetime custody for host owners whose GPU/model completion is not proven. Mirrors
// recording_has_uncertain_owners(): a retained owner is never destroyed while the process
// lives and the coordinator must not unload libobs on its account. Custody deliberately does
// not run the owner destructor: the previous host code leaked these owners with release().
class ModelOwnership final {
    static std::vector<void*>& owners() { static std::vector<void*> value; return value; }
    ModelOwnership()=delete;
public:
    // Takes custody of one exact host owner (readback custody or the retained model graph).
    static void retain(void* owner) { if(owner)owners().push_back(owner); }
    static bool unresolved() { return !owners().empty(); }
    static size_t retained() { return owners().size(); }
};
// The normal model cleanup path is admissible only when the lane actually published its
// sample receipts, every one of them settled, AND no host owner was retained unresolved. A
// lane that published no sample proves nothing about the graph and may not shut down.
inline bool model_cleanup_settled(bool publishedSamples,bool samplesSettled) noexcept {
    return publishedSamples && samplesSettled && !ModelOwnership::unresolved();
}
// Post-submission custody: the executed readback's exact owner objects (target resource,
// allocator, command list, fence, event) move into process-lifetime custody whenever
// completion was not exactly confirmed. Returns true when the owner was retained, and the
// caller must then report unresolved instead of running the settled cleanup.
template<class Owner>
bool settle_readback_owner(bool executed,HRESULT signal,uint64_t observed,uint64_t required,
    std::unique_ptr<Owner>& owner) {
    if(classify_readback(executed,signal,observed,required)==ReadbackCompletion::Confirmed)return false;
    ModelOwnership::retain(owner.release());
    return true;
}
// Failed-lane handoff: the whole lane owner is retained for the process even when no sample
// published an observed HANDLE, so a null retained HANDLE never authorises an unload.
template<class Owner>
void retain_unresolved_model(std::unique_ptr<Owner>& owner) {
    ModelOwnership::retain(owner.release());
}
// Coordinator query mirroring recording_has_uncertain_owners().
inline bool model_has_unresolved_owners() { return ModelOwnership::unresolved(); }
// A CORE-control lane shares readback() with the model lane: while any QA readback owner is
// unresolved the GPU may still read the lane's source/context, so no close step may run.
inline bool core_close_admissible() noexcept { return !ModelOwnership::unresolved(); }
// Folded into main.cpp's existing 'retained' flag beside recording_has_uncertain_owners().
inline bool fold_model_retained(bool retained) noexcept { return model_has_unresolved_owners() || retained; }
} // namespace nr::qa
