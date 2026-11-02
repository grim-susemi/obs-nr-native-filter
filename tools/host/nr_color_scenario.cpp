// SPDX-License-Identifier: GPL-3.0-only
#include "nr_color_scenario.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <iostream>
#ifdef NR_COLOR_WITH_OBS
#include "interop.h"
#include <obs.h>
#include <tlhelp32.h>
#endif
namespace nr::qa {
namespace {
using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
void require(bool b,const char* text) { if(!b) throw std::runtime_error(text); }
void check(HRESULT hr) { require(SUCCEEDED(hr),"color scenario Direct3D operation failed"); }
using V=std::array<double,3>;
// Independent double-precision XYZ oracle. Matrices here express RGB->XYZ D65,
// not the production direct Rec2020<->709 matrices or its float transfer code.
V xyz(V c,bool wide) {
    if(wide) return {0.6369580483*c[0]+0.1446169036*c[1]+0.1688809752*c[2],
        0.2627002120*c[0]+0.6779980715*c[1]+0.0593017165*c[2],1.0609850577*c[2]+0.0280726930*c[1]};
    return {0.4123907993*c[0]+0.3575843394*c[1]+0.1804807884*c[2],
        0.2126390059*c[0]+0.7151686788*c[1]+0.0721923154*c[2],
        0.0193308187*c[0]+0.1191947798*c[1]+0.9505321522*c[2]};
}
V rgb(V c,bool wide) {
    if(wide) return {1.7166511880*c[0]-0.3556707838*c[1]-0.2533662814*c[2],
        -0.6666843518*c[0]+1.6164812366*c[1]+0.0157685458*c[2],
        0.0176398574*c[0]-0.0427706133*c[1]+0.9421031212*c[2]};
    return {3.2409699419*c[0]-1.5373831776*c[1]-0.4986107603*c[2],
        -0.9692436363*c[0]+1.8759675015*c[1]+0.0415550574*c[2],
        0.0556300797*c[0]-0.2039769589*c[1]+1.0569715142*c[2]};
}
double eotf(double code) {
    const double n=std::pow(code,1.0/78.84375);
    return 10000*std::pow(std::max(n-0.8359375,0.0)/(18.8515625-18.6875*n),1.0/0.1593017578125);
}
double oetf(double nits) {
    const double n=std::pow(std::clamp(nits/10000,0.0,1.0),0.1593017578125);
    return std::pow((0.8359375+18.8515625*n)/(1+18.6875*n),78.84375);
}
ColorPixel oracle_prepare(const ColorPlan& plan,ColorPixel p) {
    if(plan.source.alpha==AlphaMode::Premultiplied&&p.a==0) return {0,0,0,p.a};
    V v{p.r,p.g,p.b};
    if(plan.source.alpha==AlphaMode::Premultiplied) for(auto& x:v) x/=p.a;
    if(plan.source.color==ColorSpace::Rec2020Pq) for(auto& x:v) x=eotf(x);
    if(plan.source.color==ColorSpace::Rec2020Linear||plan.source.color==ColorSpace::Rec2020Pq) v=rgb(xyz(v,true),false);
    return {static_cast<float>(v[0]*plan.linearScale),static_cast<float>(v[1]*plan.linearScale),
        static_cast<float>(v[2]*plan.linearScale),p.a};
}
ColorPixel oracle_restore(const ColorPlan& plan,ColorPixel raw,ColorPixel resolved) {
    if(plan.source.alpha==AlphaMode::Premultiplied&&raw.a==0) return raw;
    const auto prep=oracle_prepare(plan,raw);
    const auto encode=[&](ColorPixel p) {
        V v{p.r/plan.linearScale,p.g/plan.linearScale,p.b/plan.linearScale};
        if(plan.source.color==ColorSpace::Rec2020Linear||plan.source.color==ColorSpace::Rec2020Pq) v=rgb(xyz(v,false),true);
        if(plan.source.color==ColorSpace::Rec2020Pq) for(auto& x:v) x=oetf(x);
        return v;
    };
    const auto a=encode(prep),b=encode(resolved);
    float* channels[]={&raw.r,&raw.g,&raw.b};
    const double factor=plan.source.alpha==AlphaMode::Premultiplied?raw.a:1;
    for(size_t i=0;i<3;++i) if(*channels[i]>=0) {
        double value=*channels[i]+factor*(b[i]-a[i]);
        if(plan.source.color==ColorSpace::Rec709Srgb||plan.source.color==ColorSpace::Rec2020Pq) value=std::clamp(value,0.0,factor);
        *channels[i]=static_cast<float>(value);
    }
    return raw;
}
uint32_t bytes_per_pixel(DXGI_FORMAT f) { return f==DXGI_FORMAT_R16G16B16A16_FLOAT?8u:4u; }
ColorPixel quantize(ColorPixel p,DXGI_FORMAT f) {
    float* v[]={&p.r,&p.g,&p.b,&p.a};
    for(size_t i=0;i<4;++i) {
        if(f==DXGI_FORMAT_R16G16B16A16_FLOAT) *v[i]=XMConvertHalfToFloat(XMConvertFloatToHalf(*v[i]));
        else { const float codes=f==DXGI_FORMAT_R10G10B10A2_UNORM?(i==3?3.0f:1023.0f):255.0f;
            *v[i]=std::round(*v[i]*codes)/codes; }
    }
    return p;
}
void pack(std::byte* out,ColorPixel p,DXGI_FORMAT f) {
    if(f==DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const uint16_t h[]={XMConvertFloatToHalf(p.r),XMConvertFloatToHalf(p.g),XMConvertFloatToHalf(p.b),XMConvertFloatToHalf(p.a)};
        std::memcpy(out,h,8);
    } else if(f==DXGI_FORMAT_R10G10B10A2_UNORM) {
        const uint32_t u=static_cast<uint32_t>(std::round(p.r*1023))|
            (static_cast<uint32_t>(std::round(p.g*1023))<<10)|(static_cast<uint32_t>(std::round(p.b*1023))<<20)|
            (static_cast<uint32_t>(std::round(p.a*3))<<30);std::memcpy(out,&u,4);
    } else {
        const uint8_t u[]={static_cast<uint8_t>(std::round((f==DXGI_FORMAT_B8G8R8A8_UNORM?p.b:p.r)*255)),
            static_cast<uint8_t>(std::round(p.g*255)),static_cast<uint8_t>(std::round((f==DXGI_FORMAT_B8G8R8A8_UNORM?p.r:p.b)*255)),
            static_cast<uint8_t>(std::round(p.a*255))};std::memcpy(out,u,4);
    }
}
ColorPixel unpack(const std::byte* in,DXGI_FORMAT f) {
    if(f==DXGI_FORMAT_R16G16B16A16_FLOAT) {uint16_t h[4];std::memcpy(h,in,8);
        return {XMConvertHalfToFloat(h[0]),XMConvertHalfToFloat(h[1]),XMConvertHalfToFloat(h[2]),XMConvertHalfToFloat(h[3])};}
    uint32_t u;std::memcpy(&u,in,4);
    if(f==DXGI_FORMAT_R10G10B10A2_UNORM) return {static_cast<float>(u&1023)/1023,static_cast<float>((u>>10)&1023)/1023,
        static_cast<float>((u>>20)&1023)/1023,static_cast<float>(u>>30)/3};
    const float r=static_cast<float>(u&255)/255,b=static_cast<float>((u>>16)&255)/255;
    return {f==DXGI_FORMAT_B8G8R8A8_UNORM?b:r,static_cast<float>((u>>8)&255)/255,
        f==DXGI_FORMAT_B8G8R8A8_UNORM?r:b,static_cast<float>(u>>24)/255};
}
void barrier(ID3D12GraphicsCommandList* cmd,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cmd->ResourceBarrier(1,&b);
}
struct Run {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    std::vector<ComPtr<ID3D12Resource>> resources;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12Fence> fence;
    std::unique_ptr<D3D12ColorPass> pass;
    HANDLE event=nullptr;bool submitted=false,completed=false;
    ~Run() {if(event) CloseHandle(event);}
    ID3D12Resource* create(D3D12_RESOURCE_DESC desc,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES props{};props.Type=type;ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));
        resources.push_back(r);return r.Get();
    }
    ID3D12Resource* buffer(uint64_t size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;
        d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
        return create(d,type,state);
    }
};
// One bounded failure owner; a failed QA action never starts another generation.
thread_local Run* retainedRun=nullptr;
uint64_t image_hash(const std::vector<ColorPixel>& pixels,DXGI_FORMAT format) {
    uint64_t hash=14695981039346656037ull;std::array<std::byte,8> bytes{};
    for(const auto& pixel:pixels) {
        pack(bytes.data(),pixel,format);
        for(uint32_t i=0;i<bytes_per_pixel(format);++i) {hash^=std::to_integer<uint8_t>(bytes[i]);hash*=1099511628211ull;}
    }
    return hash;
}
struct Result {std::vector<ColorPixel> pixels;uint32_t invalid;bool refused=false;};
Result dispatch(ID3D12Device* device,ID3D12CommandQueue* queue,std::span<const std::byte> shader,
    const ColorPlan& plan,DXGI_FORMAT rawFormat,const std::vector<ColorPixel>& raw,
    const std::vector<ColorPixel>& canonical,bool restore,std::ostream* evidence=nullptr,uint32_t serial=0,
    bool refuseFormat=false,bool referenceValid=true) {
    auto run=std::make_unique<Run>();run->device=device;run->queue=queue;
    // Cleanup registered in Run before any list submission. On uncertain physical
    // completion retain the complete owner, including queue/list/upload/PSO/event.
    try {
        run->event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(run->event!=nullptr,"color event creation failed");
        run->pass=std::make_unique<D3D12ColorPass>(device,shader);
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&run->allocator)));
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,run->allocator.Get(),nullptr,IID_PPV_ARGS(&run->list)));
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&run->fence)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&run->heap)));
        const auto texture=[&](DXGI_FORMAT format,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags) {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=16;d.Height=8;
            d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=flags;
            return run->create(d,D3D12_HEAP_TYPE_DEFAULT,state);
        };
        auto* input=texture(rawFormat,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto* model=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        const DXGI_FORMAT outputFormat=refuseFormat?DXGI_FORMAT_R32_FLOAT:restore?
            (rawFormat==DXGI_FORMAT_B8G8R8A8_UNORM?DXGI_FORMAT_R8G8B8A8_UNORM:rawFormat):DXGI_FORMAT_R16G16B16A16_FLOAT;
        auto* output=texture(outputFormat,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto upload=[&](ID3D12Resource* target,const std::vector<ColorPixel>& pixels) {
            const auto d=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint64_t size=0;
            device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&size);
            auto* staging=run->buffer(size,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr;D3D12_RANGE empty{0,0};check(staging->Map(0,&empty,&mapped));
            for(size_t y=0;y<8;++y) for(size_t x=0;x<16;++x)
                pack(static_cast<std::byte*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch+x*bytes_per_pixel(d.Format),pixels[y*16+x],d.Format);
            staging->Unmap(0,nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=target;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=staging;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
            run->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            barrier(run->list.Get(),target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };
        upload(input,raw);upload(model,canonical);
        auto* status=run->buffer(4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto* zero=run->buffer(4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr;D3D12_RANGE empty{0,0};check(zero->Map(0,&empty,&mapped));std::memset(mapped,0,4);zero->Unmap(0,nullptr);
        run->list->CopyBufferRegion(status,0,zero,0,4);
        barrier(run->list.Get(),status,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool recorded=run->pass->record(run->list.Get(),run->heap.Get(),plan,input,model,output,status,restore,referenceValid);
        if(refuseFormat) {
            require(!recorded,"unavailable output format admitted");
            run.reset();
            if(evidence) *evidence<<"NR_COLOR_CONTROL {\"name\":\"unavailable-output-format\",\"stage\":\"record\",\"rejected\":true,\"submitted\":false}"<<std::endl;
            return {{},0,true};
        }
        require(recorded,"production color record refused");
        barrier(run->list.Get(),output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(run->list.Get(),status,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto od=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint64_t size=0;
        device->GetCopyableFootprints(&od,0,1,0,&fp,nullptr,nullptr,&size);
        auto* readback=run->buffer(size,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto* statusRead=run->buffer(4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        run->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);run->list->CopyBufferRegion(statusRead,0,status,0,4);
        const auto inputDesc=input->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT rawFp{};uint64_t rawBytes=0;
        device->GetCopyableFootprints(&inputDesc,0,1,0,&rawFp,nullptr,nullptr,&rawBytes);
        auto* rawRead=run->buffer(rawBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(run->list.Get(),input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        dst.pResource=rawRead;dst.PlacedFootprint=rawFp;src.pResource=input;
        run->list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier(run->list.Get(),input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        barrier(run->list.Get(),model,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        barrier(run->list.Get(),output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        barrier(run->list.Get(),status,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        check(run->list->Close());
        check(run->fence->SetEventOnCompletion(1,run->event)); // subscribe BEFORE trigger
        ID3D12CommandList* lists[]={run->list.Get()};run->submitted=true;queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(run->fence.Get(),1));
        require(WaitForSingleObject(run->event,10000)==WAIT_OBJECT_0,"color fence completion timeout; ownership retained");
        require(run->fence->GetCompletedValue()!=UINT64_MAX&&run->fence->GetCompletedValue()>=1,"color physical completion invalid");
        run->completed=true;Result result{std::vector<ColorPixel>(128),0,false};D3D12_RANGE range{0,static_cast<SIZE_T>(size)};
        check(readback->Map(0,&range,&mapped));
        for(size_t y=0;y<8;++y) for(size_t x=0;x<16;++x) result.pixels[y*16+x]=
            unpack(static_cast<const std::byte*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch+x*bytes_per_pixel(outputFormat),outputFormat);
        readback->Unmap(0,&empty);range={0,4};check(statusRead->Map(0,&range,&mapped));
        std::memcpy(&result.invalid,mapped,4);statusRead->Unmap(0,&empty);
        std::vector<ColorPixel> actualRaw(128);range={0,static_cast<SIZE_T>(rawBytes)};check(rawRead->Map(0,&range,&mapped));
        for(size_t y=0;y<8;++y) for(size_t x=0;x<16;++x) actualRaw[y*16+x]=
            unpack(static_cast<const std::byte*>(mapped)+rawFp.Offset+y*rawFp.Footprint.RowPitch+x*bytes_per_pixel(rawFormat),rawFormat);
        rawRead->Unmap(0,&empty);const auto rawHash=image_hash(actualRaw,rawFormat);
        require(rawHash==image_hash(raw,rawFormat),"immutable GPU source upload identity mismatch");
        const auto completed=run->fence->GetCompletedValue();
        const auto inputId=reinterpret_cast<uintptr_t>(input),canonicalId=reinterpret_cast<uintptr_t>(model);
        const auto outputId=reinterpret_cast<uintptr_t>(output),statusId=reinterpret_cast<uintptr_t>(status);
        const auto heapId=reinterpret_cast<uintptr_t>(run->heap.Get());
        run.reset(); // actual cleanup before the row asserts cleanup
        if(evidence) *evidence<<"NR_COLOR_DISPATCH {\"serial\":"<<serial<<",\"restore\":"<<(restore?"true":"false")
            <<",\"reference_valid\":"<<(referenceValid?"true":"false")
            <<",\"width\":16,\"height\":8,\"source_format\":"<<rawFormat<<",\"output_format\":"<<outputFormat
            <<",\"source_resource\":\""<<inputId<<"\",\"canonical_resource\":\""<<canonicalId<<"\",\"output_resource\":\""<<outputId
            <<"\",\"status_resource\":\""<<statusId<<"\",\"heap\":\""<<heapId<<"\",\"status_cleared\":true,\"invalid_status\":"<<result.invalid
            <<",\"subscribed_before_trigger\":true,\"fence_signaled\":1,\"fence_completed\":"<<completed
            <<",\"raw_expected_hash\":\""<<image_hash(raw,rawFormat)<<"\",\"raw_readback_hash\":\""<<rawHash
            <<"\",\"output_hash\":\""<<image_hash(result.pixels,outputFormat)<<"\",\"common_handoff\":true,\"cleanup\":true}"<<std::endl;
        return result;
    } catch(...) {
        if(run->submitted&&!run->completed) {
            retainedRun=run.release();
            if(evidence) *evidence<<"NR_COLOR_RETAINED serial="<<serial<<" unresolved=true"<<std::endl;
        }
        throw;
    }
}
uint32_t half_distance(float a,float b) {
    const auto ordered=[](float x) {const auto h=XMConvertFloatToHalf(x);
        return (h&0x8000)?0x8000-static_cast<int>(h&0x7fff):0x8000+static_cast<int>(h);};
    return static_cast<uint32_t>(std::abs(ordered(a)-ordered(b)));
}
void compare(ColorProof& proof,ColorPixel got,ColorPixel expected,DXGI_FORMAT format) {
    expected=quantize(expected,format);
    require(got.a==expected.a,"source alpha changed beyond declared format");
    const float g[]={got.r,got.g,got.b},e[]={expected.r,expected.g,expected.b};
    for(size_t k=0;k<3;++k) {
        if(format==DXGI_FORMAT_R16G16B16A16_FLOAT) {
            const auto ulps=half_distance(g[k],e[k]);proof.maxLinearHalfUlps=std::max(proof.maxLinearHalfUlps,ulps);
            if(ulps>2) {
                char message[256];std::snprintf(message,sizeof(message),
                    "independent linear oracle exceeds two half ULP: case=%u pixel_count=%u channel=%zu got=%.9g expected=%.9g ulps=%u",
                    proof.cases,proof.pixels,k,g[k],e[k],ulps);throw std::runtime_error(message);
            }
        } else {
            const float codes=std::abs(g[k]-e[k])*(format==DXGI_FORMAT_R10G10B10A2_UNORM?1023.0f:255.0f);
            proof.maxNormalizedCodes=std::max(proof.maxNormalizedCodes,codes);
            require(std::round(codes)<=1,"independent normalized oracle exceeds one output code");
        }
    }
    ++proof.pixels;
}
}
ColorProof run_color_scenario(ID3D12Device* device,ID3D12CommandQueue* queue,std::span<const std::byte> shader,std::ostream* evidence) {
    require(device&&queue&&!shader.empty(),"explicit device/queue/shader required");
    require(retainedRun==nullptr,"previous physical color ownership unresolved");
    require(queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT,"direct queue required");
    ComPtr<ID3D12Device> owner;check(queue->GetDevice(IID_PPV_ARGS(&owner)));
    require(owner.Get()==device,"queue/device mismatch");ColorProof proof{};uint32_t serial=0;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options)));
    require(options.DoublePrecisionFloatShaderOps,"actual basic-double shader capability unavailable");
    const auto supported=[&](DXGI_FORMAT format,bool store) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT cap{format,D3D12_FORMAT_SUPPORT1_NONE,D3D12_FORMAT_SUPPORT2_NONE};
        check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&cap,sizeof(cap)));
        return store?(cap.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)!=0:
            (cap.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)!=0;
    };
    for(const auto color:{ColorSpace::Rec709Srgb,ColorSpace::ScRgbLinear,ColorSpace::Rec2020Linear,ColorSpace::Rec2020Pq}) {
        const std::vector<PixelFormat> formats=color==ColorSpace::Rec709Srgb?
            std::vector<PixelFormat>{PixelFormat::Rgba8,PixelFormat::Bgra8}:
            std::vector<PixelFormat>{color==ColorSpace::Rec2020Pq?PixelFormat::Rgb10A2:PixelFormat::Rgba16Float};
        for(const auto format:formats) for(const auto alpha:{AlphaMode::Opaque,AlphaMode::Straight,AlphaMode::Premultiplied}) {
            const auto dxgi=color==ColorSpace::Rec709Srgb?(format==PixelFormat::Bgra8?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM):
                color==ColorSpace::Rec2020Pq?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT;
            const FrameDescriptor d{{16,8},{0,0,16,8},format,color,alpha,203};
            const auto outputFormat=dxgi==DXGI_FORMAT_B8G8R8A8_UNORM?DXGI_FORMAT_R8G8B8A8_UNORM:dxgi;
            const bool sourceLoad=supported(dxgi,false),canonicalLoad=supported(DXGI_FORMAT_R16G16B16A16_FLOAT,false);
            const bool canonicalStore=supported(DXGI_FORMAT_R16G16B16A16_FLOAT,true),outputStore=supported(outputFormat,true);
            const auto plan=color_plan(d,{2,3},sourceLoad&&canonicalLoad&&canonicalStore&&outputStore);
            require(plan.has_value(),"actual source/canonical/output capability admission failed");
            std::vector<ColorPixel> raw(128),canonical(128),expected(128);
            for(size_t i=0;i<128;++i) {
                const float ramp=static_cast<float>(i%16)/15;
                const float a=alpha==AlphaMode::Opaque?1.0f:static_cast<float>((i/16)%4)/3;
                const float premul=alpha==AlphaMode::Premultiplied?a:1.0f;
                ColorPixel p{ramp*0.8f,0.2f+ramp*0.3f,0.1f+ramp*0.6f,a};
                if(color==ColorSpace::ScRgbLinear||color==ColorSpace::Rec2020Linear) {
                    p.r=i%5==0?-0.125f:ramp*8;p.g=0.25f+ramp*2;p.b=0.5f+ramp*12;
                }
                p.r*=premul;p.g*=premul;p.b*=premul;
                if(a==0&&alpha==AlphaMode::Premultiplied) p.r=0.125f; // hidden transparent RGB preserved
                raw[i]=quantize(p,dxgi);expected[i]=oracle_prepare(*plan,raw[i]);
                canonical[i]=quantize(expected[i],DXGI_FORMAT_R16G16B16A16_FLOAT);
            }
            const uint32_t prepareSerial=++serial;
            const auto prepared=dispatch(device,queue,shader,*plan,dxgi,raw,canonical,false,evidence,prepareSerial);
            if(prepared.invalid!=0) {
                char message[128];std::snprintf(message,sizeof(message),"valid preparation marked invalid: color=%u alpha=%u status=%u",
                    static_cast<unsigned>(color),static_cast<unsigned>(alpha),prepared.invalid);throw std::runtime_error(message);
            }
            ColorProof caseProof{proof.cases,0,0,0};
            for(size_t i=0;i<128;++i) compare(caseProof,prepared.pixels[i],expected[i],DXGI_FORMAT_R16G16B16A16_FLOAT);
            const uint32_t identitySerial=++serial;
            const auto identity=dispatch(device,queue,shader,*plan,dxgi,raw,prepared.pixels,true,evidence,identitySerial);
            require(identity.invalid==0,"identity restore invalid");
            for(size_t i=0;i<128;++i) require(identity.pixels[i]==raw[i],"unchanged canonical half roundtrip changed original RGB/alpha");
            for(auto& p:canonical) {p.r*=0.95f;p.g*=0.95f;p.b*=0.95f;p=quantize(p,DXGI_FORMAT_R16G16B16A16_FLOAT);}
            const uint32_t restoreSerial=++serial;
            const auto restored=dispatch(device,queue,shader,*plan,dxgi,raw,canonical,true,evidence,restoreSerial);
            require(restored.invalid==0,"valid restore marked invalid");
            const uint32_t protectedSerial=++serial;
            const auto protectedRaw=dispatch(device,queue,shader,*plan,dxgi,raw,canonical,true,evidence,protectedSerial,false,false);
            require(protectedRaw.invalid==0&&protectedRaw.pixels==raw,"actual cold/reset shader g=0 changed source RGB/alpha");
            std::vector<ColorPixel> oracleRestored(128);uint32_t negativeChecks=0;
            for(size_t i=0;i<128;++i) {
                oracleRestored[i]=oracle_restore(*plan,raw[i],canonical[i]);
                compare(caseProof,restored.pixels[i],oracleRestored[i],dxgi);
                const float r[]={raw[i].r,raw[i].g,raw[i].b},o[]={restored.pixels[i].r,restored.pixels[i].g,restored.pixels[i].b};
                for(size_t k=0;k<3;++k) if(r[k]<0) {++negativeChecks;require(r[k]==o[k],"original negative linear channel changed");}
            }
            if(evidence) *evidence<<"NR_COLOR_CASE {\"case\":"<<proof.cases<<",\"color\":"<<static_cast<unsigned>(color)
                <<",\"format\":"<<static_cast<unsigned>(format)<<",\"alpha\":"<<static_cast<unsigned>(alpha)
                <<",\"width\":16,\"height\":8,\"source_white_nits\":203,\"pre_exposure\":2,\"exposure\":3"
                <<",\"linear_scale\":"<<plan->linearScale<<",\"white_point\":"<<plan->whitePoint
                <<",\"source_load\":"<<(sourceLoad?"true":"false")<<",\"canonical_load\":"<<(canonicalLoad?"true":"false")
                <<",\"canonical_store\":"<<(canonicalStore?"true":"false")<<",\"output_store\":"<<(outputStore?"true":"false")
                <<",\"basic_double\":"<<(options.DoublePrecisionFloatShaderOps?"true":"false")
                <<",\"prepare_serial\":"<<prepareSerial<<",\"identity_serial\":"<<identitySerial<<",\"restore_serial\":"<<restoreSerial
                <<",\"compared_pixels\":"<<caseProof.pixels<<",\"identity_pixels\":128,\"alpha_errors\":0,\"negative_checks\":"<<negativeChecks
                <<",\"normalized_codes\":"<<caseProof.maxNormalizedCodes<<",\"half_ulps\":"<<caseProof.maxLinearHalfUlps
                <<",\"raw_hash\":\""<<image_hash(raw,dxgi)<<"\",\"identity_hash\":\""<<image_hash(identity.pixels,outputFormat)
                <<"\",\"raw_output_hash\":\""<<image_hash(raw,outputFormat)
                <<"\",\"prepared_hash\":\""<<image_hash(prepared.pixels,DXGI_FORMAT_R16G16B16A16_FLOAT)
                <<"\",\"oracle_prepared_hash\":\""<<image_hash(expected,DXGI_FORMAT_R16G16B16A16_FLOAT)
                <<"\",\"restored_hash\":\""<<image_hash(restored.pixels,outputFormat)
                <<"\",\"oracle_restored_hash\":\""<<image_hash(oracleRestored,outputFormat)<<"\"}"<<std::endl;
            proof.pixels+=caseProof.pixels;
            proof.maxNormalizedCodes=std::max(proof.maxNormalizedCodes,caseProof.maxNormalizedCodes);
            proof.maxLinearHalfUlps=std::max(proof.maxLinearHalfUlps,caseProof.maxLinearHalfUlps);
            ++proof.cases;
        }
    }
    const FrameDescriptor linear{{16,8},{0,0,16,8},PixelFormat::Rgba16Float,ColorSpace::ScRgbLinear,AlphaMode::Straight,80};
    const auto plan=color_plan(linear,{1,1},true);
    std::vector<ColorPixel> invalid(128,{1,1,1,1}),model(128,{1,1,1,1});
    invalid[0].r=std::numeric_limits<float>::quiet_NaN();
    const auto invalidControl=[&](const char* name,const ColorPlan& p,DXGI_FORMAT format,const std::vector<ColorPixel>& input,
        const std::vector<ColorPixel>& resolved,bool restore,uint32_t expectedStatus,bool referenceValid=true) {
        const auto id=++serial;const auto result=dispatch(device,queue,shader,p,format,input,resolved,restore,evidence,id,false,referenceValid);
        require(result.invalid==expectedStatus,"invalid GPU status does not deny publication with exact reason");
        if(evidence) *evidence<<"NR_COLOR_CONTROL {\"name\":\""<<name<<"\",\"stage\":\"completed_gpu\",\"serial\":"<<id
            <<",\"invalid_status\":"<<result.invalid<<",\"rejected\":true,\"published\":false}"<<std::endl;
    };
    invalidControl("nonfinite-input",*plan,DXGI_FORMAT_R16G16B16A16_FLOAT,invalid,model,false,1);
    invalid[0]={1,1,1,1};model[0].g=std::numeric_limits<float>::infinity();
    invalidControl("nonfinite-resolve",*plan,DXGI_FORMAT_R16G16B16A16_FLOAT,invalid,model,true,8);
    invalidControl("nonfinite-protected-resolve",*plan,DXGI_FORMAT_R16G16B16A16_FLOAT,invalid,model,true,8,false);
    const auto exposed=color_plan(linear,{1,2},true);invalid.assign(128,{65504,65504,65504,1});
    model.assign(128,{1,1,1,1});
    invalidControl("canonical-overflow",*exposed,DXGI_FORMAT_R16G16B16A16_FLOAT,invalid,model,false,4);
    const auto lowerExposure=color_plan(linear,{4,1},true);model.assign(128,{65504,65504,65504,1});
    invalidControl("restore-overflow",*lowerExposure,DXGI_FORMAT_R16G16B16A16_FLOAT,invalid,model,true,16);
    const FrameDescriptor premul{{16,8},{0,0,16,8},PixelFormat::Rgba8,ColorSpace::Rec709Srgb,AlphaMode::Premultiplied,80};
    const auto premulPlan=color_plan(premul,{1,1},true);
    invalid.assign(128,{0.9f,0.2f,0.1f,0.5f});model.assign(128,{1,1,1,1});
    invalidControl("malformed-premultiplication",*premulPlan,DXGI_FORMAT_R8G8B8A8_UNORM,invalid,model,false,2);
    const FrameDescriptor unknown{{16,8},{0,0,16,8},PixelFormat::Rgba16Float,ColorSpace::Unknown,AlphaMode::Straight,80};
    require(!color_plan(unknown,{1,1},true),"unknown primaries admitted");
    require(!color_plan(linear,{1,1},false),"unavailable format admitted");
    if(evidence) {
        *evidence<<"NR_COLOR_CONTROL {\"name\":\"unknown-primaries\",\"stage\":\"plan\",\"rejected\":true,\"submitted\":false}"<<std::endl;
        *evidence<<"NR_COLOR_CONTROL {\"name\":\"unavailable-format-capability\",\"stage\":\"plan\",\"rejected\":true,\"submitted\":false}"<<std::endl;
    }
    invalid.assign(128,{1,1,1,1});
    require(dispatch(device,queue,shader,*plan,DXGI_FORMAT_R16G16B16A16_FLOAT,invalid,model,false,evidence,0,true).refused,
        "unavailable actual output view accepted");
    return proof;
}
}
#ifdef NR_COLOR_WITH_OBS
void nr_color_refuse_unrelated() {
    const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE) throw std::runtime_error("color process admission snapshot failed");
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);DWORD other=0;
    if(!Process32FirstW(snapshot,&entry)) {CloseHandle(snapshot);throw std::runtime_error("color process admission enumeration failed");}
    do {
        if(entry.th32ProcessID==GetCurrentProcessId()) continue;
        for(const auto name:{L"obs64.exe",L"nr-qa-host.exe",L"CrimsonDesert.exe",L"GTA5.exe",L"GTA5_Enhanced.exe",L"Cyberpunk2077.exe",
            L"eldenring.exe",L"BlackMythWukong.exe",L"cs2.exe",L"dota2.exe"})
            if(_wcsicmp(entry.szExeFile,name)==0) other=entry.th32ProcessID;
        if(_wcsnicmp(entry.szExeFile,L"FortniteClient",14)==0) other=entry.th32ProcessID;
    } while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    if(other) throw std::runtime_error("color unrelated active process refused PID="+std::to_string(other));
}
bool nr_run_color_scenario(const std::filesystem::path& root) {
    std::cout<<"NR_COLOR_CLEANUP_REGISTERED owned_context_queues_events_resources_only"<<std::endl;
    std::shared_ptr<nr::interop::Context> context;
    gs_texture_t* fixture=nullptr;bool graphicsEntered=false;
    try {
        std::ifstream file(root/"bin/64bit/nr_color.dxil",std::ios::binary|std::ios::ate);
        if(!file||file.tellg()<=0) throw std::runtime_error("actual production color DXIL missing");
        const auto count=file.tellg();std::vector<std::byte> bytes(static_cast<size_t>(count));
        file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),count);
        if(!file) throw std::runtime_error("color DXIL read failed");
        nr_color_refuse_unrelated(); // immediately before private physical device creation
        obs_enter_graphics();graphicsEntered=true;
        if(gs_get_device_type()!=GS_DEVICE_DIRECT3D_11) throw std::runtime_error("color requires actual renderer D3D11");
        fixture=gs_texture_create(16,8,GS_BGRA,1,nullptr,GS_DYNAMIC);
        if(!fixture) throw std::runtime_error("color renderer identity texture unavailable");
        auto* borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(fixture));
        if(!borrowed) throw std::runtime_error("color renderer object identity unavailable");
        Microsoft::WRL::ComPtr<ID3D11Device> renderer;borrowed->GetDevice(&renderer);
        context=nr::interop::Context::create(renderer.Get());
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||
            FAILED(factory->EnumAdapterByLuid(context->luid(),IID_PPV_ARGS(&adapter)))) throw std::runtime_error("color adapter identity unavailable");
        DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter->GetDesc1(&desc))) throw std::runtime_error("color adapter description unavailable");
        if(desc.VendorId!=0x10de||std::wstring(desc.Description).find(L"RTX 4090")==std::wstring::npos||
            (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) throw std::runtime_error("color requires physical RTX4090");
        const auto worker=context->device()->GetAdapterLuid();
        if(!nr::interop::same_luid(worker,context->luid())) throw std::runtime_error("color private same-LUID proof failed");
        std::cout<<"NR_COLOR_CONTEXT {\"renderer_luid_high\":"<<context->luid().HighPart<<",\"renderer_luid_low\":"<<context->luid().LowPart
            <<",\"worker_luid_high\":"<<worker.HighPart<<",\"worker_luid_low\":"<<worker.LowPart
            <<",\"vendor_id\":"<<desc.VendorId<<",\"device_id\":"<<desc.DeviceId
            <<",\"adapter\":\"RTX 4090\",\"software\":false,\"private_direct_queue\":true,\"device\":\""
            <<reinterpret_cast<uintptr_t>(context->device())<<"\",\"queue\":\""<<reinterpret_cast<uintptr_t>(context->queue())<<"\"}"<<std::endl;
        gs_texture_destroy(fixture);fixture=nullptr;obs_leave_graphics();graphicsEntered=false;
        // All finite passes and subscribed waits are outside the OBS graphics lock.
        const auto proof=nr::qa::run_color_scenario(context->device(),context->queue(),bytes,&std::cout);
        context.reset();
        std::cout<<"NR_COLOR_SUMMARY {\"cases\":"<<proof.cases<<",\"compared_pixels\":"<<proof.pixels
            <<",\"normalized_codes\":"<<proof.maxNormalizedCodes<<",\"half_ulps\":"<<proof.maxLinearHalfUlps
            <<",\"identity_pixels\":1920,\"cleanup\":true}"<<std::endl;
        std::cout<<"NR_COLOR_SCOPE physical_color=true model=not_loaded Feature18=not_admitted HDR=boundary_only recording=not_tested cleanup=complete"<<std::endl;
        return true;
    } catch(const std::exception& error) {
        if(fixture) gs_texture_destroy(fixture);
        if(graphicsEntered) obs_leave_graphics();
        std::cout<<"NR_COLOR_FAILURE "<<error.what()<<std::endl;
        if(nr::qa::retainedRun) {
            // The action has already failed its bounded wait. Preserve the exact
            // host/code/context until its already-subscribed physical event arrives.
            // Foreign driver hangs are outside containment; runner must NOT kill it.
            std::cout<<"NR_COLOR_RETAINED unresolved=true"<<std::endl;
            auto completed=nr::qa::retainedRun->fence->GetCompletedValue();
            while(completed==UINT64_MAX||completed<1) {
                // Exact already-subscribed event, not timed polling. No safe
                // publication/unload while a foreign driver cannot prove retirement.
                WaitForSingleObject(nr::qa::retainedRun->event,INFINITE);
                completed=nr::qa::retainedRun->fence->GetCompletedValue();
            }
            delete nr::qa::retainedRun;nr::qa::retainedRun=nullptr;
        }
        return false;
    }
}
#endif
