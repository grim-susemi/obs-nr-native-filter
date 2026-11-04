// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <d3d12.h>
namespace nr::model {
// Pool key and allocation contract. Slots0..4 preserve the legacy native path:
// canonical raw, proxy, keep, A, resolved; slots5..7 are workProxy, B, C.
struct ColorLayout {
    const Extent native,work;
    const bool multipass;
    bool operator==(const ColorLayout&) const=default;
    std::array<Extent,8> extents() const {
        const Extent absent{0,0};
        return {native,native,native,work,native,native!=work?work:absent,
            multipass?work:absent,multipass?work:absent};
    }
    std::array<D3D12_RESOURCE_STATES,8> initial_states() const {
        constexpr auto write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,common=D3D12_RESOURCE_STATE_COMMON;
        return {write,write,write,native!=work || multipass?common:write,write,common,common,common};
    }
};
// Successful record + color restore: only the final A/B answer is NPSR.
// Entry/reuse: all allocated colors UAV (reused slabs transition COMMON->UAV).
// Exit: each allocated color transitions from this table to COMMON.
// Fresh new work resources start COMMON, so a discarded list leaves them COMMON.
// Legacy five native UAV allocations stay unchanged. Refused/unsubmitted slabs never reenter the pool.
inline std::array<D3D12_RESOURCE_STATES,8> final_color_states(uint32_t completed) {
    constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return {read,read,read,completed%2?read:write,read,read,completed%2?write:read,write};
}
}
