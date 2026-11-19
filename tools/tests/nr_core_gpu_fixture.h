// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define NOMINMAX
#include "nr_core.h"
#include <DirectXPackedVector.h>
#include <dxgi1_4.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace core_gpu {
using namespace nr;
using Microsoft::WRL::ComPtr;
using Pixel=std::array<float,4>;
using Image=std::vector<Pixel>;
inline void require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
inline void check(HRESULT hr) { require(SUCCEEDED(hr),"D3D12 fixture operation"); }
inline std::vector<std::byte> bytes(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);require(f.good(),"fixture shader missing");
    const auto size=f.tellg();require(size>0,"fixture shader empty");
    std::vector<std::byte> result(static_cast<size_t>(size));f.seekg(0);
    f.read(reinterpret_cast<char*>(result.data()),size);require(f.good(),"fixture shader read");return result;
}
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    std::vector<int> trace;
    Gpu() {
        check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;check(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)));
        DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc));
        require(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"hardware fixture requires physical adapter");
        std::printf("adapter=%ls vendor=%04x device=%04x\n",desc.Description,desc.VendorId,desc.DeviceId);
        D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    }
    ComPtr<ID3D12DescriptorHeap> heap(UINT count) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors=count;d.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> h;check(device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&h)));return h;
    }
    ComPtr<ID3D12Resource> texture(Extent size) {
        D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=size.width;d.Height=size.height;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)));return r;
    }
    ComPtr<ID3D12Resource> buffer(UINT64 size,D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES p{};p.Type=type;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;
        d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,
            type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&r)));return r;
    }
    void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint(ID3D12Resource* r) {
        const auto d=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT p{};
        device->GetCopyableFootprints(&d,0,1,0,&p,nullptr,nullptr,nullptr);return p;
    }
    void upload(ID3D12Resource* r,const Image& image) {
        const auto p=footprint(r);
        auto b=buffer(UINT64(p.Footprint.RowPitch)*p.Footprint.Height,D3D12_HEAP_TYPE_UPLOAD);
        void* memory=nullptr;D3D12_RANGE empty{};check(b->Map(0,&empty,&memory));
        for(UINT y=0;y<p.Footprint.Height;++y) {
            auto* row=reinterpret_cast<uint16_t*>(static_cast<std::byte*>(memory)+y*p.Footprint.RowPitch);
            for(UINT x=0;x<p.Footprint.Width;++x) for(UINT c=0;c<4;++c)
                row[x*4+c]=DirectX::PackedVector::XMConvertFloatToHalf(image[y*p.Footprint.Width+x][c]);
        }
        b->Unmap(0,nullptr);barrier(r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=b.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint=p;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier(r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uploads.push_back(std::move(b));
    }
    Image finish(ID3D12Resource* output) {
        const auto p=footprint(output);
        auto read=buffer(UINT64(p.Footprint.RowPitch)*p.Footprint.Height,D3D12_HEAP_TYPE_READBACK);
        barrier(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint=p;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);check(list->Close());
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"GPU completion event");
        check(fence->SetEventOnCompletion(1,event)); // Subscribe before submission.
        ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),1));
        if(WaitForSingleObject(event,60000)!=WAIT_OBJECT_0) {
            std::puts("NR_CORE_GPU_RETAINED: physical completion unknown; retaining all GPU objects");std::fflush(stdout);
            WaitForSingleObject(event,INFINITE); // Never release uncompleted GPU resources.
            CloseHandle(event);throw std::runtime_error("GPU completion exceeded 60 seconds");
        }
        CloseHandle(event);check(device->GetDeviceRemovedReason());
        void* memory=nullptr;check(read->Map(0,nullptr,&memory));
        Image result(size_t(p.Footprint.Width)*p.Footprint.Height);
        for(UINT y=0;y<p.Footprint.Height;++y) {
            const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const std::byte*>(memory)+y*p.Footprint.RowPitch);
            for(UINT x=0;x<p.Footprint.Width;++x) for(UINT c=0;c<4;++c)
                result[y*p.Footprint.Width+x][c]=DirectX::PackedVector::XMConvertHalfToFloat(row[x*4+c]);
        }
        D3D12_RANGE empty{};read->Unmap(0,&empty);return result;
    }
};
struct Case {uint32_t scale=100,passes=1;float gain=0,strength=1;bool hdr=true;};
inline JobContract job(Extent size,Case c) {
    const FrameKey fk{7,1,1};
    const FrameDescriptor d{size,{0,0,size.width,size.height},c.hdr?PixelFormat::Rgba16Float:PixelFormat::Rgba8,
        c.hdr?ColorSpace::ScRgbLinear:ColorSpace::Rec709Srgb,AlphaMode::Straight,80};
    const Settings s{{1},true,"fake-evaluator-only",{c.strength},core_default_tuning(),c.passes,c.scale};
    return {{fk,1,1,1},{fk,2,1,1,1,1,SourceValidity::Valid,d,s},3};
}
}
