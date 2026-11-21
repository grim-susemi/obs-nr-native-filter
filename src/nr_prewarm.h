// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"

namespace nr::model {
struct PrewarmKey {
    const uint32_t luidLow;
    const int32_t luidHigh;
    const std::string runtimeSha;
    const uint64_t source, device, generation;
    const FrameDescriptor frame;
    const Extent work, output;
    const Settings settings;
    bool operator==(const PrewarmKey&) const=default;
};
struct PrewarmRequest {
    const PrewarmKey key;
    const uint64_t history;
    const bool inactive;
};
inline bool can_adopt_prewarm(const PrewarmRequest& prepared,const PrewarmRequest& offered,
    const PrewarmKey& actual,uint64_t history,bool bothComplete)
{
    // A cut may advance history without changing the creation contract. No
    // speculative evaluation occurred, so the first real job still resets it.
    return bothComplete && prepared.inactive && !offered.inactive &&
        prepared.key==actual && offered.key==actual &&
        offered.history==history && prepared.history<=history;
}
}
