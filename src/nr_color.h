// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <d3d12.h>
#include <wrl/client.h>
namespace nr {
struct ColorPixel {
    float r,g,b,a;
    bool operator==(const ColorPixel&) const = default;
};
// Linear Rec2020 units are reference-white units. scRGB units are always 80 nits;
// PQ is absolute ST2084 (10000 nits). SDR is already display-referred sRGB.
struct ColorExposure { float preExposure, exposure; };
struct ColorPlan {
    FrameDescriptor source;
    float linearScale,whitePoint;
    ColorSpace codecColor;
    AlphaMode codecAlpha;
};
// available means source sampling AND declared output typed store are admitted
// by the owning device. No device initialization or capability invention here.
std::optional<ColorPlan> color_plan(const FrameDescriptor&,ColorExposure,bool available);
std::optional<ColorPixel> color_prepare(const ColorPlan&,ColorPixel);
std::optional<ColorPixel> color_restore(const ColorPlan&,ColorPixel original,ColorPixel resolvedCanonical,
    bool referenceValid=true);
// The model proxy remains the frozen nr_codec encode/resolve, not this pass and
// not OFA's independently derived BGRA8 analysis. Original source never aliases
// canonical/model/analysis/output. Full active rectangle only, no downscale.
struct ColorConstants {
    uint32_t mode,color,alpha,width;
    uint32_t height,originX,originY,referenceValid;
    float linearScale; float reserved[3]{};
};
static_assert(sizeof(ColorConstants)==48);
static_assert(static_cast<uint32_t>(ColorSpace::Rec709Srgb)==1&&static_cast<uint32_t>(ColorSpace::Rec2020Pq)==4);
static_assert(static_cast<uint32_t>(AlphaMode::Premultiplied)==3);
ColorConstants color_constants(const ColorPlan&,bool restore,bool referenceValid=true);
// Caller owns resources/list/heap and physical completion. No submit/wait/readback.
// Inputs NON_PIXEL_SHADER_RESOURCE, output UAV; state handoff belongs to transport.
class D3D12ColorPass final {
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
public:
    // Actual production artifact is nr_color.dxil (SM6); DXBC audit is not admitted.
    D3D12ColorPass(ID3D12Device*,std::span<const std::byte> shader);
    bool record(ID3D12GraphicsCommandList*,ID3D12DescriptorHeap*,const ColorPlan&,
        ID3D12Resource* original,ID3D12Resource* canonical,ID3D12Resource* output,
        ID3D12Resource* invalidStatus,bool restore,bool referenceValid=true);
    // invalidStatus is a per-job four-byte UAV cleared before prepare.
    // Nonzero denies publication; completion/observation belongs to Task11.
};
}
