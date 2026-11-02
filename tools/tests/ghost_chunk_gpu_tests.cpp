// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_sha_reference.h"
#include "../host/nr_ghost_chunk.h"
#include "../host/nr_ghost_storage.h"
#include "../host/nr_ghost_scenario.h"
#include "../host/nr_bootstrap_images.h"
#include "interop.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <psapi.h>

namespace {
using Microsoft::WRL::ComPtr;
namespace digest=nr::qa::ghost_digest;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void checked(HRESULT hr,const char* why){require(SUCCEEDED(hr),why);}
D3D12_RESOURCE_DESC bytes(uint64_t n,bool uav=false) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width=(n+3)&~3ull;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
    d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(uav)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}
struct Proof {
    ComPtr<IDXGIAdapter3> adapter;
    ComPtr<ID3D11Device> renderer;
    ComPtr<ID3D11DeviceContext> immediate;
    std::shared_ptr<nr::interop::Context> context;
    std::unique_ptr<digest::ChunkPipeline> pipeline;
    digest::Sha256 root;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> scratch,source,upload;
    std::array<ComPtr<ID3D12Resource>,3> targets,images,leaves;
    nr::qa::GhostCredits credits;
    std::array<std::optional<nr::qa::GhostCredits::Token>,3> tokens;
    std::vector<uint8_t> logical;
    HANDLE event=nullptr;
    bool pending=false,mapOpen=false;
    uint64_t completion=0,ingress=0,cases=0,logicalBytes=0;
    std::array<uint64_t,3> high{};
    explicit Proof(const char* shader) {
        const LUID admitted{398289484,1};ComPtr<IDXGIFactory4> factory;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"chunk proof factory");
        checked(factory->EnumAdapterByLuid(admitted,IID_PPV_ARGS(&adapter)),"chunk proof admitted LUID");
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL selected{};
        checked(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,1,D3D11_SDK_VERSION,
            &renderer,&selected,&immediate),"chunk proof actual renderer");
        context=nr::interop::Context::create(renderer.Get());
        require(nr::interop::same_luid(context->luid(),admitted),"chunk proof actual private lane LUID");
        std::ifstream file(shader,std::ios::binary|std::ios::ate);
        require(file.good()&&file.tellg()>0,"chunk proof bound actual shader");
        std::vector<std::byte> code(static_cast<size_t>(file.tellg()));file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()),static_cast<std::streamsize>(code.size()));
        require(file.good(),"chunk proof complete actual shader bytes");
        pipeline=std::make_unique<digest::ChunkPipeline>(context->device(),code);
        auto* device=context->device();
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"chunk proof allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
            IID_PPV_ARGS(&commands)),"chunk proof list");checked(commands->Close(),"chunk proof initial close");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"chunk proof real fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"chunk proof exact event");
        scratch=resource(bytes(((16588800ull+4095)/4096)*32+4,true),
            D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    ~Proof(){if(event)CloseHandle(event);}
    ComPtr<ID3D12Resource> resource(const D3D12_RESOURCE_DESC& d,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;ComPtr<ID3D12Resource> out;
        checked(context->device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,
            IID_PPV_ARGS(&out)),"bounded actual chunk proof resource");return out;
    }
    void transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};commands->ResourceBarrier(1,&barrier);
    }
    void begin() {
        require(!pending&&!mapOpen,"chunk proof completion before allocator reuse");
        checked(allocator->Reset(),"chunk proof completed allocator reuse");
        checked(commands->Reset(allocator.Get(),nullptr),"chunk proof completed list reuse");
    }
    void submit() {
        checked(commands->Close(),"chunk proof list close");require(completion<UINT64_MAX-1,"chunk proof real fence exhaustion");
        ++completion;checked(fence->SetEventOnCompletion(completion,event),"chunk event before trigger");
        ID3D12CommandList* list[]{commands.Get()};pending=true;context->queue()->ExecuteCommandLists(1,list);
        checked(context->queue()->Signal(fence.Get(),completion),"chunk proof real signal");
        require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0&&fence->GetCompletedValue()!=UINT64_MAX&&
            fence->GetCompletedValue()>=completion,"chunk proof exact completion");pending=false;
    }
    void sample() {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonlocal{};PROCESS_MEMORY_COUNTERS_EX cpu{};cpu.cb=sizeof(cpu);
        checked(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local),"chunk proof local usage");
        checked(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonlocal),"chunk proof nonlocal usage");
        require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&cpu),sizeof(cpu))!=FALSE,
            "chunk proof private usage");
        high[0]=std::max(high[0],local.CurrentUsage);high[1]=std::max(high[1],nonlocal.CurrentUsage);
        high[2]=std::max(high[2],static_cast<uint64_t>(cpu.PrivateUsage));
    }
    void retire_images() {
        require(!pending&&!mapOpen&&credits.occupied()==0,"chunk proof full consumer retirement");
        for(auto& r:targets)r.Reset();for(auto& r:images)r.Reset();for(auto& r:leaves)r.Reset();
        source.Reset();upload.Reset();std::vector<uint8_t>{}.swap(logical);
    }
    digest::Digest verify(const nr::qa::GhostImageRows& view,const void* hashes,bool half) {
        const uint64_t length=view.logical_bytes(),leafBytes=((length+4095)/4096)*32;
        std::vector<uint8_t> referenceLeaves;referenceLeaves.reserve(static_cast<size_t>(leafBytes));
        for(uint64_t at=0,index=0;at<length;at+=4096,++index) {
            const auto count=static_cast<uint32_t>(std::min<uint64_t>(4096,length-at));
            std::vector<uint8_t> message{0};
            const auto append=[&](uint64_t value,unsigned n){for(unsigned j=0;j<n;++j)message.push_back(static_cast<uint8_t>(value>>(j*8)));};
            append(length,8);append(index,8);append(4096,4);append(count,4);
            for(uint32_t j=0;j<count;++j)message.push_back(view.row(static_cast<size_t>((at+j)/view.rowBytes))[
                static_cast<size_t>((at+j)%view.rowBytes)]);
            const auto expected=nr::qa::test::reference_sha256(message);
            require(memcmp(expected.data(),static_cast<const uint8_t*>(hashes)+index*32,32)==0,
                "actual GPU leaf differs from independent full storage-byte scalar");
            referenceLeaves.insert(referenceLeaves.end(),expected.begin(),expected.end());
        }
        std::vector<uint8_t> rootMessage{1};
        const auto append=[&](uint64_t value,unsigned n){for(unsigned j=0;j<n;++j)rootMessage.push_back(static_cast<uint8_t>(value>>(j*8)));};
        append(length,8);append(4096,4);append((length+4095)/4096,8);
        rootMessage.insert(rootMessage.end(),referenceLeaves.begin(),referenceLeaves.end());
        const auto expectedRoot=nr::qa::test::reference_sha256(rootMessage);
        const auto actualRoot=digest::chunk_root(root,length,{static_cast<const uint8_t*>(hashes),static_cast<size_t>(leafBytes)});
        require(expectedRoot==actualRoot,"actual full ordered root differs from independent scalar");
        uint32_t invalid=0;memcpy(&invalid,static_cast<const uint8_t*>(hashes)+leafBytes,4);
        bool cpuFinite=true;
        if(half)for(size_t y=0;y<view.rows;++y)cpuFinite=nr::qa::bootstrap::finite_half(view.row(y))&&cpuFinite;
        require(invalid==static_cast<uint32_t>(half&&!cpuFinite),"complete actual storage/CPU/GPU finite flag disagreement");
        for(size_t change:{size_t{0},referenceLeaves.size()/2,referenceLeaves.size()-1}) {
            referenceLeaves[change]^=1;require(digest::chunk_root(root,length,referenceLeaves)!=actualRoot,
                "corrupt first/interior/final leaf accepted");referenceLeaves[change]^=1;
        }
        if(referenceLeaves.size()>32) {
            std::swap_ranges(referenceLeaves.begin(),referenceLeaves.begin()+32,referenceLeaves.end()-32);
            require(digest::chunk_root(root,length,referenceLeaves)!=actualRoot,"reordered actual leaves accepted");
        }
        return actualRoot;
    }
    void nonfinite() {
        // Every nonfinite binary16 encoding, singly in each actual RGBA
        // channel. An aggregate OR over all encodings cannot prove this.
        constexpr uint64_t pixels=2048*4,length=pixels*8;
        logical.assign(static_cast<size_t>(length),0);
        for(uint32_t encoding=0;encoding<2048;++encoding)for(uint32_t channel=0;channel<4;++channel) {
            const uint16_t value=static_cast<uint16_t>(0x7c00|(encoding&1023)|((encoding&1024)<<5));
            memcpy(logical.data()+(encoding*4+channel)*8+channel*2,&value,2);
        }
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width=128;td.Height=64;td.DepthOrArraySize=td.MipLevels=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto layout=nr::qa::ghost_texture_bytes(context->device(),td);
        require(layout.rowBytes==1024&&layout.footprint.Footprint.RowPitch==1024&&layout.physical_bytes()==length,
            "nonfinite actual RGBA texture footprint");
        source=resource(td,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        upload=resource(bytes(length),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        targets[0]=resource(bytes(length),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
        images[0]=resource(bytes(length),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        leaves[0]=resource(bytes(pixels*36),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto token=credits.reserve({41,0xA17,++ingress},1);require(token.has_value(),"nonfinite real credit");
        tokens[0].emplace(*token);
        const D3D12_RANGE none{0,0};void* mapped=nullptr;
        checked(upload->Map(0,&none,&mapped),"nonfinite actual upload map");memcpy(mapped,logical.data(),logical.size());upload->Unmap(0,nullptr);
        begin();D3D12_TEXTURE_COPY_LOCATION from{},to{};
        from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layout.footprint;
        to.pResource=source.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(targets[0].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        from={};from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to={};to.pResource=targets[0].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=layout.footprint;
        commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(targets[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(images[0].Get(),0,targets[0].Get(),0,length);
        transition(targets[0].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(uint32_t pixel=0;pixel<pixels;++pixel) {
            pipeline->record(commands.Get(),targets[0].Get(),scratch.Get(),8,8,8,pixel*8,true);
            transition(scratch.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyBufferRegion(leaves[0].Get(),pixel*36,scratch.Get(),0,36);
            transition(scratch.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        transition(targets[0].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        sample();submit();sample();
        const D3D12_RANGE imageRange{0,static_cast<SIZE_T>(length)},leafRange{0,static_cast<SIZE_T>(pixels*36)};
        void* actual=nullptr;void* hashes=nullptr;
        checked(images[0]->Map(0,&imageRange,&actual),"nonfinite completed actual image map");mapOpen=true;
        try {
            checked(leaves[0]->Map(0,&leafRange,&hashes),"nonfinite completed leaves map");
            try {
                require(memcmp(actual,logical.data(),logical.size())==0,"nonfinite actual texture storage bits changed");
                for(size_t pixel=0;pixel<pixels;++pixel) {
                    const nr::qa::GhostImageRows one{{static_cast<const uint8_t*>(actual)+pixel*8,8},8,8,1};
                    require(!nr::qa::bootstrap::finite_half(one.tight()),"nonfinite scalar binary16 control omitted");
                    (void)verify(one,static_cast<const uint8_t*>(hashes)+pixel*36,true);
                }
            }catch(...){leaves[0]->Unmap(0,&none);throw;}
            leaves[0]->Unmap(0,&none);
        }catch(...){images[0]->Unmap(0,&none);mapOpen=false;throw;}
        images[0]->Unmap(0,&none);mapOpen=false;
        require(credits.release(*tokens[0],{completion,fence->GetCompletedValue(),completion,fence->GetCompletedValue(),
            true,true,true,true}),"nonfinite complete CPU/unmap/credit retirement");
        logicalBytes+=length;++cases;
        std::printf("GPU_CHUNK_NONFINITE encodings=2048 channels=4 individual_actual_pixels=8192 every_GPU_flag_CPU_scan_leaf_root_exact=true alpha_signedzero_tail_preserved=true credits=0 normal_release=true native_model=false\n");
        retire_images();
    }
    digest::Digest codec(uint32_t seed,bool ten,bool planar,bool reordered=false) {
        nr::qa::DecodedFrame frame{};frame.width=1920;frame.height=1080;
        frame.format=ten?(planar?VIDEO_FORMAT_I010:VIDEO_FORMAT_P010):(planar?VIDEO_FORMAT_I420:VIDEO_FORMAT_NV12);
        const uint32_t b=ten?2u:1u;
        frame.rowBytes=planar?std::array<uint32_t,3>{1920*b,960*b,960*b}:std::array<uint32_t,3>{1920*b,1920*b,0};
        frame.rows=planar?std::array<uint32_t,3>{1080,540,540}:std::array<uint32_t,3>{1080,540,0};
        logical.clear();
        for(size_t plane=0;plane<3;++plane) {
            auto& storage=frame.planes[plane];storage.resize(static_cast<size_t>(frame.rowBytes[plane])*frame.rows[plane]);
            for(size_t at=0;at<storage.size();++at)storage[at]=static_cast<uint8_t>(at*37+(at>>12)+seed+plane*53);
        }
        const std::array<size_t,3> order=reordered?std::array<size_t,3>{1,0,2}:std::array<size_t,3>{0,1,2};
        for(auto plane:order)logical.insert(logical.end(),frame.planes[plane].begin(),frame.planes[plane].end());
        const auto length=logical.size(),leafBytes=((length+4095)/4096)*32;
        upload=resource(bytes(length),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        const D3D12_RANGE none{0,0};void* mapped=nullptr;
        checked(upload->Map(0,&none,&mapped),"codec actual owned-plane upload map");
        memcpy(mapped,logical.data(),length);upload->Unmap(0,nullptr);
        digest::Digest firstRoot{};
        for(size_t slot=0;slot<3;++slot) {
            images[slot]=resource(bytes(length),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            leaves[slot]=resource(bytes(leafBytes+4),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto token=credits.reserve({41,seed,++ingress},1);require(token.has_value(),"codec three credits");
            tokens[slot].emplace(*token);
        }
        require(!credits.reserve({41,seed,ingress+1},1),"codec fourth credit refused");
        begin();
        for(size_t slot=0;slot<3;++slot) {
            commands->CopyBufferRegion(images[slot].Get(),0,upload.Get(),0,length);
            pipeline->record(commands.Get(),upload.Get(),scratch.Get(),static_cast<uint32_t>(length),
                static_cast<uint32_t>(length),static_cast<uint32_t>(length),0,false);
            transition(scratch.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyBufferRegion(leaves[slot].Get(),0,scratch.Get(),0,leafBytes+4);
            transition(scratch.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        sample();submit();sample();
        for(size_t slot=0;slot<3;++slot) {
            const D3D12_RANGE imageRange{0,length},leafRange{0,leafBytes+4};void* actual=nullptr;void* hashes=nullptr;
            checked(images[slot]->Map(0,&imageRange,&actual),"codec completed owned-plane byte map");mapOpen=true;
            try {
                checked(leaves[slot]->Map(0,&leafRange,&hashes),"codec completed leaf map");
                try {
                    require(memcmp(actual,logical.data(),length)==0,"codec complete actual ordered plane bytes changed");
                    const auto value=verify({{static_cast<const uint8_t*>(actual),length},length,length,1},hashes,false);
                    if(slot==0)firstRoot=value;
                    std::printf("GPU_CHUNK_CODEC seed=%u ten=%u planar=%u reordered=%u credit=%zu bytes=%zu root=%s chunks_cross_plane_boundaries=true owned_planes=true recording_evidence=false\n",
                        seed,static_cast<unsigned>(ten),static_cast<unsigned>(planar),static_cast<unsigned>(reordered),slot,length,digest::hex(value).c_str());
                }catch(...){leaves[slot]->Unmap(0,&none);throw;}
                leaves[slot]->Unmap(0,&none);
            }catch(...){images[slot]->Unmap(0,&none);mapOpen=false;throw;}
            images[slot]->Unmap(0,&none);mapOpen=false;
            require(credits.release(*tokens[slot],{completion,fence->GetCompletedValue(),completion,fence->GetCompletedValue(),
                true,true,true,true}),"codec full consumer/unmap/credit retirement");
        }
        logicalBytes+=length*3;++cases;retire_images();return firstRoot;
    }
    void image(uint32_t seed,unsigned mode,size_t role,DXGI_FORMAT format,UINT width=1920,UINT height=1080,
        uint32_t offset=0,unsigned corruption=0) {
        D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width=width;td.Height=height;td.DepthOrArraySize=td.MipLevels=1;td.SampleDesc.Count=1;td.Format=format;
        const auto layout=nr::qa::ghost_texture_bytes(context->device(),td);
        const auto span=layout.physical_bytes(),length=layout.logical_bytes(),leafBytes=((length+4095)/4096)*32;
        require(length<=16588800&&length<=UINT32_MAX,"chunk proof one-role admitted scratch bound");
        const bool half=format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_R16G16_FLOAT;
        logical.resize(static_cast<size_t>(length));
        if(width==1920&&height==1080&&mode<2&&(format==DXGI_FORMAT_R8G8B8A8_UNORM||
            format==DXGI_FORMAT_B8G8R8A8_UNORM)) {
            const auto f=nr::ghost::fixture(seed,static_cast<nr::ghost::FixtureColor>(mode));
            logical=nr::qa::bootstrap::ghost_fixture_bytes(f,41,half);
            if(format==DXGI_FORMAT_B8G8R8A8_UNORM)for(size_t at=0;at<logical.size();at+=4)std::swap(logical[at],logical[at+2]);
        }else for(size_t at=0;at<logical.size();++at)logical[at]=static_cast<uint8_t>(at*37+(at>>11)+seed+role);
        if(half)for(size_t at=0;at<logical.size();at+=2) {
            uint16_t value=0;memcpy(&value,logical.data()+at,2);
            // Exhaust every finite binary16 storage encoding, independently in
            // every channel; nonfinite singleton coverage is separate below.
            value=static_cast<uint16_t>((at/(format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4)+seed+mode*37)%65536);
            if((value&0x7c00)==0x7c00)value=static_cast<uint16_t>(value&0x83ff);
            memcpy(logical.data()+at,&value,2);
        }
        if(corruption) {
            const size_t at=corruption==1?0:corruption==2?logical.size()/2:logical.size()-1;
            logical[at]^=1;
        }
        source=resource(td,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        upload=resource(bytes(offset+span),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr;const D3D12_RANGE none{0,0};
        checked(upload->Map(0,&none,&mapped),"chunk proof actual upload map");
        memset(mapped,0xed,static_cast<size_t>(upload->GetDesc().Width));
        for(UINT y=0;y<layout.rows;++y)memcpy(static_cast<uint8_t*>(mapped)+offset+
            static_cast<size_t>(y)*layout.footprint.Footprint.RowPitch,logical.data()+static_cast<size_t>(y)*layout.rowBytes,
            static_cast<size_t>(layout.rowBytes));
        upload->Unmap(0,nullptr);
        for(size_t slot=0;slot<3;++slot) {
            targets[slot]=resource(bytes(offset+span),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
            images[slot]=resource(bytes(offset+span),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            leaves[slot]=resource(bytes(leafBytes+4),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto token=credits.reserve({41,seed,++ingress},1);require(token.has_value(),"chunk proof three credits");
            tokens[slot].emplace(*token);
        }
        require(!credits.reserve({41,seed,ingress+1},1),"chunk proof fourth credit refused");
        sample();begin();
        auto placed=layout.footprint;placed.Offset=offset;
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint=placed;to.pResource=source.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(size_t slot=0;slot<3;++slot) {
            transition(targets[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            commands->CopyBufferRegion(targets[slot].Get(),0,upload.Get(),0,offset+span);
            from={};to={};from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=targets[slot].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=placed;
            commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            transition(targets[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyBufferRegion(images[slot].Get(),0,targets[slot].Get(),0,offset+span);
            transition(targets[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            pipeline->record(commands.Get(),targets[slot].Get(),scratch.Get(),static_cast<uint32_t>(length),
                static_cast<uint32_t>(layout.rowBytes),placed.Footprint.RowPitch,offset,half);
            transition(scratch.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyBufferRegion(leaves[slot].Get(),0,scratch.Get(),0,leafBytes+4);
            transition(scratch.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(targets[slot].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        }
        submit();sample();
        for(size_t slot=0;slot<3;++slot) {
            const D3D12_RANGE imageRange{0,static_cast<SIZE_T>(offset+span)},leafRange{0,static_cast<SIZE_T>(leafBytes+4)};
            void* pixels=nullptr;void* hashes=nullptr;
            checked(images[slot]->Map(0,&imageRange,&pixels),"chunk completed image map");mapOpen=true;
            try {
                checked(leaves[slot]->Map(0,&leafRange,&hashes),"chunk completed leaf map");
                try {
                    const nr::qa::GhostImageRows view{{static_cast<const uint8_t*>(pixels)+offset,static_cast<size_t>(span)},
                        static_cast<size_t>(layout.rowBytes),placed.Footprint.RowPitch,layout.rows};
                    view.validate();
                    for(size_t y=0;y<view.rows;++y)require(memcmp(view.row(y).data(),logical.data()+y*view.rowBytes,view.rowBytes)==0,
                        "chunk actual full image bits/alpha/tail changed");
                    const auto rootValue=verify(view,hashes,half);
                    std::printf("GPU_CHUNK_ROOT seed=%u mode=%u role=%zu credit=%zu algorithm=%s schema=%u root=%s frame_instance=%llu frame_epoch=%llu frame_ingress=%llu generation=%llu serial=%llu\n",
                        seed,mode,role,slot,digest::chunkAlgorithm.data(),digest::chunkSchema,digest::hex(rootValue).c_str(),
                        static_cast<unsigned long long>(tokens[slot]->frame.instanceId),static_cast<unsigned long long>(tokens[slot]->frame.contentEpoch),
                        static_cast<unsigned long long>(tokens[slot]->frame.ingressSequence),static_cast<unsigned long long>(tokens[slot]->generation),
                        static_cast<unsigned long long>(tokens[slot]->serial));
                    logicalBytes+=length;
                }catch(...){leaves[slot]->Unmap(0,&none);throw;}
                leaves[slot]->Unmap(0,&none);
            }catch(...){images[slot]->Unmap(0,&none);mapOpen=false;throw;}
            images[slot]->Unmap(0,&none);mapOpen=false;
            require(credits.release(*tokens[slot],{completion,fence->GetCompletedValue(),completion,fence->GetCompletedValue(),
                true,true,true,true}),"chunk proof real full consumer/unmap/credit retirement");
        }
        std::printf("GPU_CHUNK_CASE seed=%u mode=%u role=%zu format=%u width=%u height=%u logical=%llu pitch=%u offset=%u corruption=%u credits=0 full_bits_leaves_roots_finite=true normal_release=true native_model=false physical_quality=false\n",
            seed,mode,role,static_cast<unsigned>(format),width,height,static_cast<unsigned long long>(length),
            placed.Footprint.RowPitch,offset,corruption);
        ++cases;retire_images();
    }
};
}

int nr_ghost_chunk_gpu_proof(const char* shader) {
    std::unique_ptr<Proof> proof;
    try {
        const std::string_view abc="abc";
        require(digest::hex(nr::qa::test::reference_sha256({reinterpret_cast<const uint8_t*>(abc.data()),abc.size()}))==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","GPU proof independent SHA KAT");
        proof=std::make_unique<Proof>(shader);
        for(uint32_t seed:{0xA17u,0xB29u})for(unsigned mode=0;mode<4;++mode)for(size_t role=0;role<17;++role) {
            const bool half=mode>=2;
            const auto format=role<5?DXGI_FORMAT_R16G16B16A16_FLOAT:role==5||role==6?DXGI_FORMAT_B8G8R8A8_UNORM:
                role==7?DXGI_FORMAT_R16G16_FLOAT:role==8?DXGI_FORMAT_R16G16_SINT:role==9?DXGI_FORMAT_R8_UINT:
                half?DXGI_FORMAT_R16G16B16A16_FLOAT:role==14?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
            proof->image(seed,mode,role,format);
        }
        for(UINT n:{1u,30u,31u,32u,39u,55u,63u,64u,65u,4095u,4096u,4097u,8191u,8192u,8193u})
            proof->image(0xA17,0,9,DXGI_FORMAT_R8_UINT,n,1,512);
        for(unsigned corruption=1;corruption<=3;++corruption)
            proof->image(0xB29,0,9,DXGI_FORMAT_R8_UINT,4097,1,512,corruption);
        proof->nonfinite();
        for(uint32_t seed:{0xA17u,0xB29u})for(bool ten:{false,true})for(bool planar:{false,true}) {
            const auto ordered=proof->codec(seed,ten,planar);
            const auto reordered=proof->codec(seed,ten,planar,true);
            require(ordered!=reordered,"actual codec plane reordering omitted");
        }
        std::printf("GPU_CHUNK_PROOF_CLOSED cases=%llu logical_bytes=%llu sampled_local_high=%llu sampled_nonlocal_high=%llu sampled_private_high=%llu private_lanes=1 max_credits=3 maps=0 credits=0 actual_texture_scalar_exact=true normal_release=true native_model=false physical_quality=false service_viability=false\n",
            static_cast<unsigned long long>(proof->cases),static_cast<unsigned long long>(proof->logicalBytes),
            static_cast<unsigned long long>(proof->high[0]),static_cast<unsigned long long>(proof->high[1]),
            static_cast<unsigned long long>(proof->high[2]));
        proof.reset();return 0;
    }catch(const std::exception& error) {
        const bool retained=proof&&(proof->pending||proof->mapOpen||proof->credits.occupied()!=0);
        if(retained)(void)proof.release();
        std::fprintf(stderr,"FAIL GPU_CHUNK_PROOF retained=%u %s\n",static_cast<unsigned>(retained),error.what());return 1;
    }
}
