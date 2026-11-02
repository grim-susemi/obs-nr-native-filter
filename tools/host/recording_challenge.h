#pragma once
#include "nr_contract.h"
#include <string>
namespace nr::qa {
struct RecordingChallenge {
    const JobKey key;
    bool ready=false,eligible=false,evaluated=false,composed=false,submitted=false;
    uint32_t invalid=0;
    uintptr_t module=0;
    std::string modulePath;
    bool fence=false;
    uint64_t completionValue=0,completedValue=0;
};
inline bool genuine_recording_challenge(const RecordingChallenge& c,const JobKey& expected)
{
    return c.key==expected && c.ready && c.eligible && c.evaluated && c.composed &&
        c.submitted && c.invalid==0 && c.module && !c.modulePath.empty() && c.fence &&
        c.completionValue>0 && c.completedValue!=UINT64_MAX && c.completedValue>=c.completionValue;
}
}
