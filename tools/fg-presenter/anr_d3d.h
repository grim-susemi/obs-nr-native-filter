// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>

namespace nr::fg::vfg {
inline void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}
} // namespace nr::fg::vfg
