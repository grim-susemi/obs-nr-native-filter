// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace nr::qa {
// OBS 32.2.2 storage[0] is the outermost filter. Each target is the next
// index, so source pixels traverse before -> NR -> after at decreasing indices.
constexpr bool recording_filter_order_valid(int beforeIndex,int nrIndex,int afterIndex) noexcept {
    return beforeIndex>=0 && nrIndex>=0 && afterIndex>=0 &&
        afterIndex<nrIndex && nrIndex<beforeIndex;
}
}
