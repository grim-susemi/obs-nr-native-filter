// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <array>
#include <utility>
#include <d3d12.h>
#include <wrl/client.h>

namespace nr::qa {
// A borrowed actual image, including a padded cost/vector footprint. This never
// pretends that padded storage is a contiguous logical image or owns another copy.
struct GhostImageRows {
    std::span<const uint8_t> storage;
    size_t rowBytes=0,rowPitch=0,rows=0;
    bool empty() const {return rows==0;}
    size_t logical_bytes() const {return rowBytes*rows;}
    void validate() const {
        if(!rows) {
            if(!storage.empty()||rowBytes||rowPitch)throw std::runtime_error("absent image has storage");
            return;
        }
        if(!rowBytes||rowPitch<rowBytes||rowBytes>storage.size()||
            rows-1>(storage.size()-rowBytes)/rowPitch)throw std::runtime_error("actual pitched image extent");
    }
    std::span<const uint8_t> row(size_t y) const {
        validate();
        if(y>=rows)throw std::runtime_error("actual image row index");
        return storage.subspan(y*rowPitch,rowBytes);
    }
    std::span<const uint8_t> tight() const {
        validate();
        if(rowPitch!=rowBytes)throw std::runtime_error("padded image is not a tight span");
        return storage.first(logical_bytes());
    }
};
struct GhostTextureBytes {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows=0;
    UINT64 rowBytes=0;
    uint64_t logical_bytes() const {return rowBytes*rows;}
    uint64_t physical_bytes() const {
        return rows?static_cast<uint64_t>(rows-1)*footprint.Footprint.RowPitch+rowBytes:0;
    }
};
inline GhostTextureBytes ghost_texture_bytes(ID3D12Device* device,const D3D12_RESOURCE_DESC& description) {
    if(!device||description.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||description.MipLevels!=1||
        description.DepthOrArraySize!=1||description.SampleDesc.Count!=1)
        throw std::runtime_error("actual diagnostic texture descriptor");
    GhostTextureBytes result;
    device->GetCopyableFootprints(&description,0,1,0,&result.footprint,&result.rows,&result.rowBytes,nullptr);
    if(!result.rows||!result.rowBytes||result.footprint.Footprint.RowPitch<result.rowBytes)
        throw std::runtime_error("actual diagnostic texture footprint");
    return result;
}
inline bool ghost_copy_texture_bytes(ID3D12GraphicsCommandList* commands,ID3D12Resource* source,
    ID3D12Resource* target,const GhostTextureBytes& layout) {
    if(!commands||!source||!target)return false;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(FAILED(source->GetDevice(IID_PPV_ARGS(&device))))return false;
    const auto sourceDesc=source->GetDesc(),targetDesc=target->GetDesc();
    const auto expected=ghost_texture_bytes(device.Get(),sourceDesc);
    const auto& a=layout.footprint.Footprint;const auto& b=expected.footprint.Footprint;
    if(targetDesc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||layout.footprint.Offset!=0||
        layout.rows!=expected.rows||layout.rowBytes!=expected.rowBytes||a.Format!=b.Format||
        a.Width!=b.Width||a.Height!=b.Height||a.Depth!=b.Depth||a.RowPitch!=b.RowPitch||
        targetDesc.Width<layout.physical_bytes())return false;
    std::array<D3D12_RESOURCE_BARRIER,2> barriers{};
    for(auto& barrier:barriers){barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;}
    barriers[0].Transition.pResource=source;barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource=target;barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    commands->ResourceBarrier(static_cast<UINT>(barriers.size()),barriers.data());
    D3D12_TEXTURE_COPY_LOCATION from{},to{};
    from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=layout.footprint;
    commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    for(auto& barrier:barriers)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    commands->ResourceBarrier(static_cast<UINT>(barriers.size()),barriers.data());return true;
}
inline GhostImageRows ghost_image_rows(std::span<const uint8_t> storage,const GhostTextureBytes& layout) {
    GhostImageRows result{storage,static_cast<size_t>(layout.rowBytes),layout.footprint.Footprint.RowPitch,layout.rows};
    result.validate();return result;
}
}
