// SPDX-License-Identifier: GPL-3.0-only
#include "nr_color.h"
#include <algorithm>
#include <DirectXPackedVector.h>
#include <stdexcept>
namespace nr {
namespace {
using Triple=std::array<float,3>;
Triple to709(Triple c) {
    return {1.660491002f*c[0]-0.587641139f*c[1]-0.072849863f*c[2],
        -0.124550475f*c[0]+1.132899897f*c[1]-0.008349422f*c[2],
        -0.018150763f*c[0]-0.100578898f*c[1]+1.118729661f*c[2]};
}
Triple to2020(Triple c) {
    return {0.627403896f*c[0]+0.329283038f*c[1]+0.043313066f*c[2],
        0.069097289f*c[0]+0.919540395f*c[1]+0.011362316f*c[2],
        0.016391439f*c[0]+0.088013308f*c[1]+0.895595253f*c[2]};
}
float pq_decode(float x) {
    constexpr float m1=2610.0f/16384,m2=2523.0f/32,c1=3424.0f/4096,c2=2413.0f/128,c3=2392.0f/128;
    const double p=std::pow(static_cast<double>(x),1.0/static_cast<double>(m2));
    return static_cast<float>(10000*std::pow(std::max(p-c1,0.0)/(c2-c3*p),1.0/static_cast<double>(m1)));
}
float pq_encode(float nits) {
    constexpr float m1=2610.0f/16384,m2=2523.0f/32,c1=3424.0f/4096,c2=2413.0f/128,c3=2392.0f/128;
    const float p=std::pow(std::clamp(nits/10000,0.0f,1.0f),m1);
    return std::pow((c1+c2*p)/(1+c3*p),m2);
}
bool valid_pixel(ColorPixel p,ColorSpace color) {
    if(!std::isfinite(p.r)||!std::isfinite(p.g)||!std::isfinite(p.b)||!std::isfinite(p.a)||p.a<0||p.a>1) return false;
    return (color!=ColorSpace::Rec709Srgb && color!=ColorSpace::Rec2020Pq) ||
        (p.r>=0&&p.r<=1&&p.g>=0&&p.g<=1&&p.b>=0&&p.b<=1);
}
Triple decode(const ColorPlan& p,ColorPixel raw) {
    Triple c{raw.r,raw.g,raw.b};
    if(p.source.alpha==AlphaMode::Premultiplied && raw.a>0) for(auto& v:c) v/=raw.a;
    if(p.source.color==ColorSpace::Rec2020Pq) for(auto& v:c) v=pq_decode(v);
    if(p.source.color==ColorSpace::Rec2020Linear||p.source.color==ColorSpace::Rec2020Pq) c=to709(c);
    for(auto& v:c) v*=p.linearScale;
    return c;
}
Triple encode(const ColorPlan& p,Triple c) {
    for(auto& v:c) v/=p.linearScale;
    if(p.source.color==ColorSpace::Rec2020Linear||p.source.color==ColorSpace::Rec2020Pq) c=to2020(c);
    if(p.source.color==ColorSpace::Rec2020Pq) for(auto& v:c) v=pq_encode(v);
    return c;
}
}
std::optional<ColorPlan> color_plan(const FrameDescriptor& d,ColorExposure e,bool available) {
    if(!d.valid()||!available||!std::isfinite(e.preExposure)||e.preExposure<=0||!std::isfinite(e.exposure)||e.exposure<=0) return {};
    const bool sdr=d.color==ColorSpace::Rec709Srgb;
    const float exposure=e.exposure/e.preExposure;
    const float unit=d.color==ColorSpace::Rec2020Pq?1.0f:d.color==ColorSpace::Rec2020Linear?d.referenceWhiteNits:80.0f;
    const float scale=sdr?1.0f:unit/80*exposure;
    const float white=sdr?1.0f:d.referenceWhiteNits/80*exposure;
    if(!std::isfinite(scale)||scale<=0||!std::isfinite(white)||white<=0) return {};
    return ColorPlan{d,scale,white,sdr?ColorSpace::Rec709Srgb:ColorSpace::ScRgbLinear,
        d.alpha==AlphaMode::Opaque?AlphaMode::Opaque:AlphaMode::Straight};
}
std::optional<ColorPixel> color_prepare(const ColorPlan& p,ColorPixel raw) {
    if(!valid_pixel(raw,p.source.color)) return {};
    if((p.source.color==ColorSpace::Rec709Srgb||p.source.color==ColorSpace::Rec2020Pq)&&
        p.source.alpha==AlphaMode::Premultiplied&&raw.a>0&&(raw.r>raw.a||raw.g>raw.a||raw.b>raw.a)) return {};
    if(p.source.alpha==AlphaMode::Premultiplied && raw.a==0) return ColorPixel{0,0,0,raw.a};
    const auto c=decode(p,raw);const ColorPixel result{c[0],c[1],c[2],raw.a};
    if(!valid_pixel(result,p.codecColor)||std::abs(result.r)>65504||std::abs(result.g)>65504||std::abs(result.b)>65504) return {};
    return result;
}
std::optional<ColorPixel> color_restore(const ColorPlan& p,ColorPixel raw,ColorPixel resolved,bool referenceValid) {
    const auto prepared=color_prepare(p,raw);
    if(!prepared||!valid_pixel(resolved,ColorSpace::ScRgbLinear)) return {};
    // A finite SDR answer may overshoot [0,1] once history is carried; the GPU restore (nr_color.hlsl) clamps that
    // pixel instead of rejecting the frame, so the contract twin does the same.
    if(p.codecColor==ColorSpace::Rec709Srgb) for(float* v:{&resolved.r,&resolved.g,&resolved.b}) *v=std::clamp(*v,0.0f,1.0f);
    if(!referenceValid)return raw;
    const auto half=[](float x) {return DirectX::PackedVector::XMConvertHalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(x));};
    if((p.source.alpha==AlphaMode::Premultiplied&&raw.a==0)||
        (prepared->r==resolved.r&&prepared->g==resolved.g&&prepared->b==resolved.b)||
        (half(prepared->r)==resolved.r&&half(prepared->g)==resolved.g&&half(prepared->b)==resolved.b)) return raw;
    const auto before=encode(p,{prepared->r,prepared->g,prepared->b});
    const auto after=encode(p,{resolved.r,resolved.g,resolved.b});
    float* channels[]={&raw.r,&raw.g,&raw.b};
    const float factor=p.source.alpha==AlphaMode::Premultiplied?raw.a:1.0f;
    for(size_t i=0;i<3;++i) {
        // Preserve source negative linear channels, not a clamped proxy substitute.
        if(*channels[i]<0) continue;
        *channels[i]+=factor*(after[i]-before[i]);
        if(p.source.color==ColorSpace::Rec709Srgb||p.source.color==ColorSpace::Rec2020Pq)
            *channels[i]=std::clamp(*channels[i],0.0f,factor);
    }
    if(p.source.format==PixelFormat::Rgba16Float&&(std::abs(raw.r)>65504||std::abs(raw.g)>65504||std::abs(raw.b)>65504)) return {};
    return valid_pixel(raw,p.source.color)?std::optional<ColorPixel>(raw):std::nullopt;
}
ColorConstants color_constants(const ColorPlan& p,bool restore,bool referenceValid) {
    return {restore?1u:0u,static_cast<uint32_t>(p.source.color),static_cast<uint32_t>(p.source.alpha),
        p.source.active.width,p.source.active.height,p.source.active.x,p.source.active.y,referenceValid?1u:0u,p.linearScale,{}};
}
D3D12ColorPass::D3D12ColorPass(ID3D12Device* device,std::span<const std::byte> shader):device_(device) {
    if(!device||shader.empty()) throw std::invalid_argument("color pass requires explicit device and shader");
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options)))||!options.DoublePrecisionFloatShaderOps)
        throw std::runtime_error("color pass requires admitted double arithmetic for PQ precision");
    const auto check=[](HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("color PSO initialization failed"); };
    D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,2}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants={0,0,sizeof(ColorConstants)/4};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC desc{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
    check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root_.Get();ps.CS={shader.data(),shader.size()};
    check(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline_)));
}
bool D3D12ColorPass::record(ID3D12GraphicsCommandList* cmd,ID3D12DescriptorHeap* heap,const ColorPlan& p,
    ID3D12Resource* raw,ID3D12Resource* canonical,ID3D12Resource* output,ID3D12Resource* invalidStatus,bool restore,bool referenceValid) {
    if(!cmd||!heap||!raw||!canonical||!output||!invalidStatus||raw==canonical||raw==output||canonical==output) return false;
    const auto hd=heap->GetDesc();
    if(hd.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV||hd.NumDescriptors<4||!(hd.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return false;
    const auto sameDevice=[&](ID3D12DeviceChild* child) {
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        return SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner)))&&owner.Get()==device_.Get();
    };
    if(!sameDevice(cmd)||!sameDevice(heap)) return false;
    ID3D12Resource* resources[]={raw,canonical,output,invalidStatus};
    for(auto* resource:resources) if(!sameDevice(resource)) return false;
    const auto texture=[](D3D12_RESOURCE_DESC d,uint32_t w,uint32_t h) {
        return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==w&&d.Height==h&&
            d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1;
    };
    const auto rd=raw->GetDesc(),cd=canonical->GetDesc(),od=output->GetDesc(),sd=invalidStatus->GetDesc();
    if(!texture(rd,p.source.allocation.width,p.source.allocation.height)||
        !texture(cd,p.source.active.width,p.source.active.height)||!texture(od,p.source.active.width,p.source.active.height)||
        !(od.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)||sd.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||sd.Width<4||
        !(sd.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)||cd.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    const auto sourceFormat=[&](DXGI_FORMAT f) {
        switch(p.source.format) {
        case PixelFormat::Bgra8:return f==DXGI_FORMAT_B8G8R8A8_UNORM;
        case PixelFormat::Rgba8:return f==DXGI_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::Rgba16Float:return f==DXGI_FORMAT_R16G16B16A16_FLOAT;
        case PixelFormat::Rgb10A2:return f==DXGI_FORMAT_R10G10B10A2_UNORM;
        default:return false;
        }
    };
    const bool outputMatches=sourceFormat(od.Format)||
        (p.source.format==PixelFormat::Bgra8&&od.Format==DXGI_FORMAT_R8G8B8A8_UNORM);
    // Transport's BGRA private raw retains original bytes; its shared output
    // may be RGBA with identical numeric RGB/alpha, never an sRGB view coercion.
    if(!p.source.valid()||!sourceFormat(rd.Format)||
        (restore?!outputMatches:od.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)) return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{od.Format,D3D12_FORMAT_SUPPORT1_NONE,D3D12_FORMAT_SUPPORT2_NONE};
    if(FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)))||
        !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return false;
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
    const UINT stride=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for(UINT i=0;i<2;++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=resources[i]->GetDesc().Format;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels=1;device_->CreateShaderResourceView(resources[i],&view,cpu);cpu.ptr+=stride;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=od.Format;view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(output,nullptr,&view,cpu);cpu.ptr+=stride;
    view={};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements=1;view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(invalidStatus,nullptr,&view,cpu);
    const auto constants=color_constants(p,restore,referenceValid);
    cmd->SetDescriptorHeaps(1,&heap);cmd->SetComputeRootSignature(root_.Get());cmd->SetPipelineState(pipeline_.Get());
    cmd->SetComputeRoot32BitConstants(0,sizeof(constants)/4,&constants,0);
    cmd->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());
    cmd->Dispatch((constants.width+7)/8,(constants.height+7)/8,1);return true;
}
}
