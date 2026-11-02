// SPDX-License-Identifier: GPL-3.0-only
#include "../host/nr_ghost_storage.h"
#include "../host/nr_ghost_chunk.h"
#include "../host/nr_ghost_scenario.h"
#include "../host/nr_bootstrap_images.h"
#include "interop.h"
#include "nr_color.h"
#include "nr_core.h"
#include "nr_model.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string_view>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <psapi.h>
#include <fstream>
#include <filesystem>

namespace {
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void checked(HRESULT value,const char* why){if(FAILED(value))throw std::runtime_error(why);}
constexpr uint64_t align(uint64_t n,uint64_t a){return (n+a-1)/a*a;}
constexpr std::array<const char*,17> names{"canonical","encoded","keep","foreign","resolved",
    "current_analysis","reference_analysis","motion","vectors","cost","completed",
    "current_raw","selected_raw","selected","program","preview","projector"};
D3D12_RESOURCE_DESC texture(DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=1920;d.Height=1080;
    d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;return d;
}
D3D12_RESOURCE_DESC buffer(uint64_t bytes,bool uav=false) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=align(bytes,4);
    d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(uav)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;return d;
}
void cpu() {
    // Actual byte order, full rows, gaps and tails, not a presence-only test.
    for(size_t rowBytes:{size_t{1},size_t{55},size_t{63},size_t{64},size_t{65},size_t{480},size_t{1920},size_t{7680}}) {
        const size_t pitch=static_cast<size_t>(align(rowBytes,256)),rows=7;
        std::vector<uint8_t> padded((rows-1)*pitch+rowBytes,0xed),logical(rowBytes*rows);
        for(size_t y=0;y<rows;++y)for(size_t x=0;x<rowBytes;++x) {
            const auto v=static_cast<uint8_t>(x*37+y*53);
            padded[y*pitch+x]=logical[y*rowBytes+x]=v;
        }
        const nr::qa::GhostImageRows view{padded,rowBytes,pitch,rows};
        view.validate();expect(view.logical_bytes()==logical.size(),"logical size includes padding");
        for(size_t y=0;y<rows;++y)expect(std::memcmp(view.row(y).data(),logical.data()+y*rowBytes,rowBytes)==0,
            "CPU image consumer lost actual bytes or read row padding");
        auto rejected=[&](auto action,const char* why){bool bad=false;try{action();}catch(const std::runtime_error&){bad=true;}expect(bad,why);};
        rejected([&]{(void)view.row(rows);},"row beyond actual image accepted");
        if(pitch!=rowBytes)rejected([&]{(void)view.tight();},"padded bytes masquerade as tight image");
        else expect(view.tight().size()==logical.size(),"actual tight image unavailable");
        rejected([&]{nr::qa::GhostImageRows{std::span<const uint8_t>{padded}.first(padded.size()-1),rowBytes,pitch,rows}.validate();},
            "partial final actual row accepted");
        // Full memcmp remains possible on every tight encoded/foreign image.
        auto changed=logical;changed.back()^=1;
        expect(std::memcmp(logical.data(),changed.data(),logical.size())!=0,"full no-op comparison omitted final byte");
    }
    // Working native cost: every byte of all1080 rows is independently checked.
    constexpr size_t costRow=1920,costPitch=2048,costRows=1080;
    std::vector<uint8_t> cost((costRows-1)*costPitch+costRow,0xed),oldCompact(costRow*costRows);
    for(size_t y=0;y<costRows;++y)for(size_t x=0;x<costRow;++x) {
        const auto v=static_cast<uint8_t>(x*37+y*53);
        cost[y*costPitch+x]=v;oldCompact[y*costRow+x]=v;
    }
    const nr::qa::GhostImageRows workingCost{cost,costRow,costPitch,costRows};
    for(size_t y=0;y<costRows;++y)for(size_t x=0;x<costRow;++x)
        expect(workingCost.row(y)[x]==oldCompact[y*costRow+x],"pitched native cost differs from actual old compaction");
    expect(workingCost.row(costRows-1).back()==oldCompact.back(),"native cost final byte unavailable");
    nr::qa::GhostImageRows{}.validate();
    std::printf("PASS ghost_image_rows checks=%u queue_creation=0 gpu_dispatch=0\n",checks);
}
void query(bool replacementPlan,bool ownPool=false) {
    // Only device/resource descriptor queries and ONE shared texture per format.
    // No queue, command allocator/list, fence, shader, model, copy or dispatch.
    const LUID expected{398289484,1};
    ComPtr<IDXGIFactory4> factory;checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"allocation query factory");
    ComPtr<IDXGIAdapter> adapter;checked(factory->EnumAdapterByLuid(expected,IID_PPV_ARGS(&adapter)),"same admitted native adapter LUID");
    ComPtr<ID3D12Device> device;checked(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"allocation query device");
    ComPtr<ID3D11Device> renderer;ComPtr<ID3D11DeviceContext> immediate;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL level{};
    checked(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,1,D3D11_SDK_VERSION,
        &renderer,&level,&immediate),"matching shared-target descriptor query renderer");
    const auto allocated=[&](const D3D12_RESOURCE_DESC& d) {
        const auto info=device->GetResourceAllocationInfo(0,1,&d);
        expect(info.SizeInBytes!=UINT64_MAX&&info.SizeInBytes!=0,"allocation query unsupported descriptor");
        return info.SizeInBytes;
    };
    std::array<D3D12_RESOURCE_DESC,3> openedDescs{};
    const std::array<DXGI_FORMAT,3> graphicFormats{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT};
    for(size_t f=0;f<graphicFormats.size();++f) {
        D3D11_TEXTURE2D_DESC td{};td.Width=1920;td.Height=1080;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=graphicFormats[f];td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ComPtr<ID3D11Texture2D> sharedTexture;checked(renderer->CreateTexture2D(&td,nullptr,&sharedTexture),"actual shared diagnostic texture descriptor");
        ComPtr<IDXGIResource1> shared;checked(sharedTexture.As(&shared),"actual shared diagnostic resource");
        HANDLE handle=nullptr;checked(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),
            "actual shared diagnostic handle");
        ComPtr<ID3D12Resource> opened;const auto hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&opened));CloseHandle(handle);
        checked(hr,"actual shared diagnostic open");openedDescs[f]=opened->GetDesc();
        std::printf("GRAPHIC_ALIAS format=%u flags=%u allocation=%llu shared_api_views=2 physical_allocations=1\n",
            static_cast<unsigned>(td.Format),static_cast<unsigned>(openedDescs[f].Flags),
            static_cast<unsigned long long>(allocated(openedDescs[f])));
    }
    for(bool half:{false,true}) {
        uint64_t reported=0,actualDesc=0,modelCapacity=0,end=0,leaves=0,logicalTotal=0;
        std::vector<ComPtr<ID3D12Resource>> owned12;
        std::vector<ComPtr<ID3D11Texture2D>> owned11;
        const auto committed=[&](const D3D12_RESOURCE_DESC& desc,D3D12_HEAP_TYPE type) {
            D3D12_HEAP_PROPERTIES heap{};heap.Type=type;ComPtr<ID3D12Resource> resource;
            checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:
                    desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COMMON,
                nullptr,IID_PPV_ARGS(&resource)),"actual bounded owner allocation");
            expect(allocated(resource->GetDesc())==allocated(desc),"allocated resource descriptor capacity changed");
            owned12.push_back(std::move(resource));
        };
        for(size_t r=0;r<names.size();++r) {
            const auto format=r<5?DXGI_FORMAT_R16G16B16A16_FLOAT:r==5||r==6?DXGI_FORMAT_B8G8R8A8_UNORM:
                r==7?DXGI_FORMAT_R16G16_FLOAT:r==8?DXGI_FORMAT_R16G16_SINT:r==9?DXGI_FORMAT_R8_UINT:
                half?DXGI_FORMAT_R16G16B16A16_FLOAT:r==14?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
            const auto desc=texture(format);D3D12_PLACED_SUBRESOURCE_FOOTPRINT local{},placed{};
            UINT rows=0;UINT64 rowBytes=0,total=0;
            device->GetCopyableFootprints(&desc,0,1,0,&local,&rows,&rowBytes,&total);
            device->GetCopyableFootprints(&desc,0,1,align(end,512),&placed,nullptr,nullptr,nullptr);
            const auto width=static_cast<uint64_t>(rows-1)*local.Footprint.RowPitch+rowBytes;
            end=placed.Offset+width;const auto logical=rowBytes*rows,n=(logical+4095)/4096;
            logicalTotal+=logical;leaves+=n;
            if(!replacementPlan)reported+=allocated(desc);
            const auto physical=r<11?allocated(replacementPlan?buffer(width):desc):
                allocated(openedDescs[half?2:r==14?1:0]);
            actualDesc+=physical;if(r<11)modelCapacity+=physical;
            if(ownPool)for(unsigned credit=0;credit<3;++credit) {
                if(r<11)committed(replacementPlan?buffer(width):desc,D3D12_HEAP_TYPE_DEFAULT);
                else {
                    D3D11_TEXTURE2D_DESC td{};td.Width=1920;td.Height=1080;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
                    td.Format=format;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                    td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                    ComPtr<ID3D11Texture2D> t;checked(renderer->CreateTexture2D(&td,nullptr,&t),"actual credit graphic owner");
                    ComPtr<IDXGIResource1> shared;checked(t.As(&shared),"actual credit graphic share");
                    HANDLE handle=nullptr;checked(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,
                        nullptr,&handle),"actual credit graphic handle");
                    ComPtr<ID3D12Resource> opened;const auto hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&opened));CloseHandle(handle);
                    checked(hr,"actual credit graphic alias open");
                    expect(allocated(opened->GetDesc())==physical,"actual graphic allocation flags/capacity changed");
                    owned11.push_back(std::move(t));owned12.push_back(std::move(opened));
                }
            }
            std::printf("ROLE_ALLOCATION replacement=%u half=%u role=%s logical=%llu offset=%llu row_bytes=%llu pitch=%u rows=%u physical_span=%llu GPU=%llu leaf_count=%llu\n",
                static_cast<unsigned>(replacementPlan),static_cast<unsigned>(half),names[r],static_cast<unsigned long long>(logical),static_cast<unsigned long long>(placed.Offset),
                static_cast<unsigned long long>(rowBytes),local.Footprint.RowPitch,rows,static_cast<unsigned long long>(width),
                static_cast<unsigned long long>(physical),static_cast<unsigned long long>(n));
        }
        const auto readback=allocated(buffer(end));
        const auto leafReadback=replacementPlan?allocated(buffer(leaves*32+17*4)):0;
        const auto scratch=replacementPlan?allocated(buffer(((16588800ull+4095)/4096)*32+4,true)):0;
        std::array<std::vector<uint8_t>,3> oldCompact;
        size_t compactCapacity=0;if(!replacementPlan)for(auto& v:oldCompact){v.resize(2073600);compactCapacity+=v.capacity();}
        if(ownPool) {
            for(unsigned credit=0;credit<3;++credit) {
                committed(buffer(end),D3D12_HEAP_TYPE_READBACK);
                if(replacementPlan)committed(buffer(leaves*32+17*4),D3D12_HEAP_TYPE_READBACK);
            }
            if(replacementPlan)committed(buffer(((16588800ull+4095)/4096)*32+4,true),D3D12_HEAP_TYPE_DEFAULT);
            expect(owned11.size()==18&&owned12.size()==(replacementPlan?58u:54u),"actual owner/alias count");
            std::printf("OWNED_POOL replacement=%u half=%u distinct_model=33 distinct_graphics=18 aliases_graphics=18 image_readbacks=3 leaf_readbacks=%u leaf_scratch=%u owning12_refs=%zu compact_capacity=%llu uninitialized_pixels_not_evidence=true shader_code=not_created native_vendor_owners=not_loaded queue_creation=0 gpu_dispatch=0\n",
                static_cast<unsigned>(replacementPlan),static_cast<unsigned>(half),replacementPlan?3u:0u,replacementPlan?1u:0u,
                owned12.size(),static_cast<unsigned long long>(compactCapacity));
        }
        if(!half&&!replacementPlan)expect(reported*3==533790720&&readback*3==529268736,"native71 matching descriptor allocation baseline drift");
        std::printf("PHASE_ALLOCATION replacement=%u half=%u reported_GPU=%llu actual_opened_GPU=%llu model_GPU=%llu common_leaf_GPU=%llu phase_GPU=%llu image_READBACK=%llu leaf_READBACK=%llu compact_CPU_capacity=%llu logical_bytes_per_envelope=%llu buffer_width=%llu root_code_driver_overhead=unmeasured replacement_consumers=not_integrated capacities=runtime_query_not_full_live_pool_highwater runtime_admission=false gpu_dispatch=0\n",
            static_cast<unsigned>(replacementPlan),static_cast<unsigned>(half),static_cast<unsigned long long>(reported*3),
            static_cast<unsigned long long>(actualDesc*3),static_cast<unsigned long long>(modelCapacity*3),
            static_cast<unsigned long long>(scratch),static_cast<unsigned long long>(actualDesc*3+scratch),
            static_cast<unsigned long long>(readback*3),static_cast<unsigned long long>(leafReadback*3),
            static_cast<unsigned long long>(compactCapacity),static_cast<unsigned long long>(logicalTotal),
            static_cast<unsigned long long>(end));
        // No submitted work exists. Release aliases and owning resources before
        // the next transport, never retain baseline and replacement together.
        owned12.clear();owned11.clear();for(auto& v:oldCompact)std::vector<uint8_t>{}.swap(v);
        std::printf("PHASE_OWNER_RELEASE replacement=%u half=%u resources=0 compact_capacity=0 submitted_work=0 normal_release=true\n",
            static_cast<unsigned>(replacementPlan),static_cast<unsigned>(half));
    }
    std::printf("RESOURCE_QUERY_CLOSED shared_targets=0 queues=0 command_lists=0 gpu_dispatch=0 runtime_admission=false\n");
}
void code_query(const char* shaderPath) {
    // Empty private context/code/codec allocation only. Never submit commands.
    const LUID expected{398289484,1};
    ComPtr<IDXGIFactory4> factory;checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"code query factory");
    ComPtr<IDXGIAdapter3> adapter;
    checked(factory->EnumAdapterByLuid(expected,IID_PPV_ARGS(&adapter)),"code query exact admitted adapter");
    const auto usage=[&] {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonlocal{};PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb=sizeof(process);
        checked(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local),"local owning usage");
        checked(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonlocal),"nonlocal owning usage");
        expect(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),sizeof(process))!=FALSE,
            "code query actual process memory");
        return std::array<uint64_t,3>{local.CurrentUsage,nonlocal.CurrentUsage,process.PrivateUsage};
    };
    const auto baseline=usage();
    ComPtr<ID3D12Device> device;
    checked(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"code query private device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    checked(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"codec retained private lane");
    ComPtr<ID3D12Fence> fence;
    checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"codec retained completion fence");
    const auto context=usage();
    std::ifstream file(shaderPath,std::ios::binary|std::ios::ate);
    expect(file.good()&&file.tellg()>0,"actual source-bound shader bytes");
    const auto size=static_cast<size_t>(file.tellg());std::vector<std::byte> shader(size);
    file.seekg(0);file.read(reinterpret_cast<char*>(shader.data()),static_cast<std::streamsize>(shader.size()));
    expect(file.good(),"complete actual shader read");
    {
        nr::qa::ghost_digest::ChunkPipeline pipeline(device.Get(),shader);
        const auto code=usage();
        std::printf("CODE_OWNERS shader_bytes=%zu shader_capacity=%zu baseline_local=%llu baseline_nonlocal=%llu baseline_private=%llu context_local=%llu context_nonlocal=%llu context_private=%llu code_local=%llu code_nonlocal=%llu code_private=%llu private_devices=1 private_queues=1 fences=1 signatures=1 pipelines=1 submitted_work=0 gpu_dispatch=0 vendor_model_owners=not_loaded\n",
            shader.size(),shader.capacity(),static_cast<unsigned long long>(baseline[0]),static_cast<unsigned long long>(baseline[1]),
            static_cast<unsigned long long>(baseline[2]),static_cast<unsigned long long>(context[0]),
            static_cast<unsigned long long>(context[1]),static_cast<unsigned long long>(context[2]),
            static_cast<unsigned long long>(code[0]),static_cast<unsigned long long>(code[1]),static_cast<unsigned long long>(code[2]));
        for(bool half:{false,true}) {
            const uint64_t bytes=half?6220800ull:3110400ull,n=(bytes+4095)/4096;
            std::array<ComPtr<ID3D12Resource>,3> owners;
            const std::array<D3D12_RESOURCE_DESC,3> descriptors{buffer(bytes),buffer(n*32+4,true),buffer(n*32+4)};
            const std::array<D3D12_HEAP_TYPE,3> types{D3D12_HEAP_TYPE_UPLOAD,D3D12_HEAP_TYPE_DEFAULT,D3D12_HEAP_TYPE_READBACK};
            uint64_t upload=0,gpu=0,readback=0;
            for(size_t i=0;i<owners.size();++i) {
                D3D12_HEAP_PROPERTIES heap{};heap.Type=types[i];
                checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&descriptors[i],
                    i==0?D3D12_RESOURCE_STATE_GENERIC_READ:i==1?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,IID_PPV_ARGS(&owners[i])),"one actual owned codec frame capacity");
                const auto actual=owners[i]->GetDesc();
                const auto capacity=device->GetResourceAllocationInfo(0,1,&actual).SizeInBytes;
                if(i==0)upload=capacity;else if(i==1)gpu=capacity;else readback=capacity;
            }
            const auto live=usage();
            std::printf("CODEC_OWNERS half=%u logical_planes=%llu upload_capacity=%llu leaf_GPU=%llu leaf_READBACK=%llu local=%llu nonlocal=%llu private=%llu owned_frames=1 model_input_owners=0 submitted_work=0 gpu_dispatch=0 runtime_admission=false\n",
                static_cast<unsigned>(half),static_cast<unsigned long long>(bytes),static_cast<unsigned long long>(upload),
                static_cast<unsigned long long>(gpu),static_cast<unsigned long long>(readback),static_cast<unsigned long long>(live[0]),
                static_cast<unsigned long long>(live[1]),static_cast<unsigned long long>(live[2]));
            for(auto& owner:owners)owner.Reset();
            std::printf("CODEC_OWNER_RELEASE half=%u resources=0 submitted_work=0 normal_release=true\n",static_cast<unsigned>(half));
        }
    }
    std::vector<std::byte>{}.swap(shader);fence.Reset();queue.Reset();device.Reset();
    std::printf("CODE_QUERY_CLOSED pipelines=0 signatures=0 devices=0 queues=0 fences=0 resources=0 submitted_work=0 normal_release=true gpu_dispatch=0\n");
}
void matched_code_query(const char* shaderPath,bool replacement) {
    // Match actual persistent OBS D3D11 rendering and production code creation.
    // Baseline/replacement are separate processes, never doubled live pools.
    const LUID expected{398289484,1};
    ComPtr<IDXGIFactory4> factory;checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"matched code factory");
    ComPtr<IDXGIAdapter3> adapter;
    checked(factory->EnumAdapterByLuid(expected,IID_PPV_ARGS(&adapter)),"matched code actual native LUID");
    ComPtr<ID3D11Device> renderer;ComPtr<ID3D11DeviceContext> immediate;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL level{};
    checked(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,1,D3D11_SDK_VERSION,
        &renderer,&level,&immediate),"matched persistent actual renderer");
    const auto usage=[&](const char* phase) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonlocal{};PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb=sizeof(process);
        checked(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local),"matched local usage");
        checked(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonlocal),"matched nonlocal usage");
        expect(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),sizeof(process))!=FALSE,
            "matched process private capacity");
        std::printf("MATCHED_CODE_PHASE replacement=%u phase=%s local=%llu nonlocal=%llu private=%llu renderer=1 model_input_owners=0 vendor_model_owners=not_loaded submitted_work=0 gpu_dispatch=0\n",
            static_cast<unsigned>(replacement),phase,static_cast<unsigned long long>(local.CurrentUsage),
            static_cast<unsigned long long>(nonlocal.CurrentUsage),static_cast<unsigned long long>(process.PrivateUsage));
    };
    const auto bytes=[](const std::filesystem::path& path) {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file.good()||file.tellg()<=0)throw std::runtime_error("matched source-bound code bytes");
        std::vector<std::byte> value(static_cast<size_t>(file.tellg()));file.seekg(0);
        file.read(reinterpret_cast<char*>(value.data()),static_cast<std::streamsize>(value.size()));
        if(!file.good())throw std::runtime_error("matched complete code bytes");
        return value;
    };
    const auto directory=std::filesystem::path(shaderPath).parent_path();
    usage("persistent_renderer");
    auto context=nr::interop::Context::create(renderer.Get());
    expect(nr::interop::same_luid(context->luid(),expected),"matched actual private context LUID");
    auto colorBytes=bytes(directory/"nr_color.dxil"),codecBytes=bytes(directory/"nr_codec.dxil");
    auto color=std::make_unique<nr::D3D12ColorPass>(context->device(),colorBytes);
    auto codec=std::make_unique<nr::D3D12Codec>(context->device(),codecBytes);
    usage("ordinary_before_chunk");
    std::vector<std::byte> chunkBytes;
    std::unique_ptr<nr::qa::ghost_digest::ChunkPipeline> chunk;
    if(replacement) {
        chunkBytes=bytes(shaderPath);
        chunk=std::make_unique<nr::qa::ghost_digest::ChunkPipeline>(context->device(),chunkBytes);
        usage("ordinary_after_chunk");
        std::printf("MATCHED_CODE_CPU replacement=1 chunk_shader_capacity=%zu chunk_signatures=1 chunk_pipelines=1 root_metadata=borrowed_leaf_readback source_model_color_code_unchanged=true\n",chunkBytes.capacity());
    }
    // Actual stop_graph destroys SnapshotPool and native color/codec/model/input
    // owners before decode. Only the new needed lane/code may remain in codec.
    color.reset();codec.reset();std::vector<std::byte>{}.swap(colorBytes);std::vector<std::byte>{}.swap(codecBytes);
    if(!replacement)context.reset();
    usage(replacement?"codec_retained_lane_code":"codec_baseline_native_retired");
    if(replacement)for(bool half:{false,true}) {
        const uint64_t logical=half?6220800ull:3110400ull,n=(logical+4095)/4096;
        std::array<ComPtr<ID3D12Resource>,3> owners;
        const std::array<D3D12_RESOURCE_DESC,3> descriptors{buffer(logical),buffer(n*32+4,true),buffer(n*32+4)};
        const std::array<D3D12_HEAP_TYPE,3> types{D3D12_HEAP_TYPE_UPLOAD,D3D12_HEAP_TYPE_DEFAULT,D3D12_HEAP_TYPE_READBACK};
        std::array<uint64_t,3> capacities{};
        for(size_t i=0;i<owners.size();++i) {
            D3D12_HEAP_PROPERTIES heap{};heap.Type=types[i];
            checked(context->device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&descriptors[i],
                i==0?D3D12_RESOURCE_STATE_GENERIC_READ:i==1?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,IID_PPV_ARGS(&owners[i])),"matched one codec frame owner");
            const auto actual=owners[i]->GetDesc();
            capacities[i]=context->device()->GetResourceAllocationInfo(0,1,&actual).SizeInBytes;
        }
        usage(half?"codec_one_HDR_frame":"codec_one_SDR_frame");
        std::printf("MATCHED_CODEC_CAPACITY half=%u upload=%llu leaf_GPU=%llu leaf_READBACK=%llu frames=1 RGB_canonical_BMP_overlap=0 model_input_owners=0\n",
            static_cast<unsigned>(half),static_cast<unsigned long long>(capacities[0]),
            static_cast<unsigned long long>(capacities[1]),static_cast<unsigned long long>(capacities[2]));
        for(auto& owner:owners)owner.Reset();
        std::printf("MATCHED_CODEC_RELEASE half=%u resources=0 submitted_work=0 normal_release=true\n",static_cast<unsigned>(half));
    }
    chunk.reset();std::vector<std::byte>{}.swap(chunkBytes);context.reset();
    immediate.Reset();renderer.Reset();
    std::printf("MATCHED_CODE_CLOSED replacement=%u renderer=0 contexts=0 queues=0 signatures=0 pipelines=0 images=0 submitted_work=0 normal_release=true gpu_dispatch=0 runtime_admission=false\n",static_cast<unsigned>(replacement));
}
void copy_storage(bool modelBoundary=false) {
    // COPY ONLY. Actual storage/footprints/consumers, not NR/identity/quality
    // acceptance. Three real credit-owned targets/readbacks, one CPU drain.
    const LUID expected{398289484,1};
    ComPtr<IDXGIFactory4> factory;checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"copy-only factory");
    ComPtr<IDXGIAdapter> adapter;checked(factory->EnumAdapterByLuid(expected,IID_PPV_ARGS(&adapter)),"copy-only admitted LUID");
    ComPtr<ID3D11Device> renderer;ComPtr<ID3D11DeviceContext> immediate;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL level{};
    checked(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,1,D3D11_SDK_VERSION,
        &renderer,&level,&immediate),"copy-only actual renderer");
    auto context=nr::interop::Context::create(renderer.Get());auto* device=context->device();
    ComPtr<IDXGIAdapter3> budget;checked(adapter.As(&budget),"copy-only control allocation budget");
    const auto controlUsage=[&] {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonlocal{};PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb=sizeof(process);
        checked(budget->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local),"copy-only control local usage");
        checked(budget->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonlocal),"copy-only control nonlocal usage");
        expect(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),sizeof(process))!=FALSE,
            "copy-only control process private usage");
        return std::array<uint64_t,3>{local.CurrentUsage,nonlocal.CurrentUsage,process.PrivateUsage};
    };
    const auto controlBefore=controlUsage();
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;
    checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"copy-only allocator");
    checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"copy-only list");
    checked(commands->Close(),"copy-only initial close");
    checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"copy-only completion");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);expect(event!=nullptr,"copy-only exact event");
    struct EventOwner {HANDLE value;~EventOwner(){if(value)CloseHandle(value);}} eventOwner{event};
    const auto controlAfter=controlUsage();
    std::printf("CODEC_CONTROL_OWNER_CAPACITY allocators=1 lists=1 fences=1 events=1 before_local=%llu after_local=%llu before_nonlocal=%llu after_nonlocal=%llu before_private=%llu after_private=%llu same_process=true image_owners_at_measurement=0 submitted_work=0 GPU_dispatch=0\n",
        static_cast<unsigned long long>(controlBefore[0]),static_cast<unsigned long long>(controlAfter[0]),
        static_cast<unsigned long long>(controlBefore[1]),static_cast<unsigned long long>(controlAfter[1]),
        static_cast<unsigned long long>(controlBefore[2]),static_cast<unsigned long long>(controlAfter[2]));
    uint64_t completion=0,copiedBytes=0;
    const std::array<DXGI_FORMAT,6> formats{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16_SINT,DXGI_FORMAT_R8_UINT};
    const auto resource=[&](const D3D12_RESOURCE_DESC& description,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
        ComPtr<ID3D12Resource> value;D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
        checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,state,nullptr,IID_PPV_ARGS(&value)),
            "copy-only bounded resource");return value;
    };
    const auto transition=[&](ID3D12Resource* value,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={value,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};commands->ResourceBarrier(1,&barrier);
    };
    for(uint32_t seed:{0xA17u,0xB29u})for(const auto format:formats) {
        if(modelBoundary&&format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&format!=DXGI_FORMAT_R8_UINT)continue;
        auto description=texture(format);
        if(modelBoundary){description.Width=2560;description.Height=1440;}
        const auto layout=nr::qa::ghost_texture_bytes(device,description);
        std::vector<uint8_t> logical(static_cast<size_t>(layout.logical_bytes()));
        if(!modelBoundary&&(format==DXGI_FORMAT_R8G8B8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM||
            format==DXGI_FORMAT_R16G16B16A16_FLOAT)) {
            const auto corpus=nr::ghost::fixture(seed,nr::ghost::FixtureColor::Sdr);
            logical=nr::qa::bootstrap::ghost_fixture_bytes(corpus,41,format==DXGI_FORMAT_R16G16B16A16_FLOAT);
            if(format==DXGI_FORMAT_B8G8R8A8_UNORM)for(size_t p=0;p<logical.size();p+=4)std::swap(logical[p],logical[p+2]);
        }else for(size_t at=0;at<logical.size();++at)logical[at]=static_cast<uint8_t>(at*37+(at>>11)+seed);
        expect(logical.size()==layout.logical_bytes(),"copy-only actual logical descriptor extent");
        auto source=resource(description,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        auto upload=resource(buffer(layout.physical_bytes()),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        std::array<ComPtr<ID3D12Resource>,3> targets,readbacks;
        uint64_t targetCapacity=0,readbackCapacity=0;
        for(size_t slot=0;slot<3;++slot) {
            targets[slot]=resource(buffer(layout.physical_bytes()),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
            readbacks[slot]=resource(buffer(layout.physical_bytes()),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto targetDesc=targets[slot]->GetDesc(),readDesc=readbacks[slot]->GetDesc();
            targetCapacity+=device->GetResourceAllocationInfo(0,1,&targetDesc).SizeInBytes;
            readbackCapacity+=device->GetResourceAllocationInfo(0,1,&readDesc).SizeInBytes;
        }
        void* mapped=nullptr;const D3D12_RANGE none{0,0};checked(upload->Map(0,&none,&mapped),"copy-only owned upload map");
        std::memset(mapped,0xed,static_cast<size_t>(upload->GetDesc().Width));
        for(UINT row=0;row<layout.rows;++row)std::memcpy(static_cast<uint8_t*>(mapped)+static_cast<size_t>(row)*layout.footprint.Footprint.RowPitch,
            logical.data()+static_cast<size_t>(row)*layout.rowBytes,static_cast<size_t>(layout.rowBytes));
        upload->Unmap(0,nullptr);
        checked(allocator->Reset(),"copy-only allocator reuse");checked(commands->Reset(allocator.Get(),nullptr),"copy-only list reuse");
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();
        from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layout.footprint;
        to.pResource=source.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);transition(source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        nr::qa::GhostCredits credits;std::array<std::optional<nr::qa::GhostCredits::Token>,3> tokens{};
        for(size_t slot=0;slot<3;++slot) {
            const auto token=credits.reserve({41,seed,slot+1},1);expect(token.has_value(),"copy-only three real credits");tokens[slot].emplace(*token);
            if(modelBoundary) {
                nr::model::Observation observation;
                const nr::JobKey key{token->frame,1,1,token->serial};observation.key.emplace(key);
                const size_t role=format==DXGI_FORMAT_R8_UINT?nr::model::Observation::Cost:nr::model::Observation::Canonical;
                observation.images[role]=targets[slot];observation.imageFootprints[role]=layout.footprint;
                std::array<ID3D12Resource*,nr::model::Observation::Count> sources{};sources[role]=source.Get();
                auto wrong=layout.footprint;wrong.Offset=512;
                expect(!nr::model::observation_destination(source.Get(),targets[slot].Get(),wrong),"model local footprint offset accepted");
                wrong=layout.footprint;wrong.Footprint.RowPitch+=256;
                expect(!nr::model::observation_destination(source.Get(),targets[slot].Get(),wrong),"model incorrect row pitch accepted");
                const nr::JobKey other{token->frame,1,1,token->serial+1};
                expect(!nr::model::copy_observation(commands.Get(),observation,other,sources),"model changed immutable job key accepted");
                expect(observation.stages==0,"refused model boundary recorded a stage");
                expect(nr::model::copy_observation(commands.Get(),observation,key,sources),"actual production model byte destination refused");
                expect(observation.stages==(1ull<<role),"actual production model stage presence lost");
            }else expect(nr::qa::ghost_copy_texture_bytes(commands.Get(),source.Get(),targets[slot].Get(),layout),"actual texture to footprint buffer copy refused");
            transition(targets[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyBufferRegion(readbacks[slot].Get(),0,targets[slot].Get(),0,layout.physical_bytes());
            transition(targets[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
            copiedBytes+=layout.logical_bytes()*2;
        }
        expect(!credits.reserve({41,seed,4},1),"copy-only fourth credit admitted");
        checked(commands->Close(),"copy-only commands close");++completion;
        checked(fence->SetEventOnCompletion(completion,event),"copy-only event subscribed before submission");
        ID3D12CommandList* lists[]{commands.Get()};context->queue()->ExecuteCommandLists(1,lists);
        const auto signal=context->queue()->Signal(fence.Get(),completion);
        const auto wait=SUCCEEDED(signal)?WaitForSingleObject(event,30000):WAIT_FAILED;
        const auto done=fence->GetCompletedValue();
        if(FAILED(signal)||wait!=WAIT_OBJECT_0||done==UINT64_MAX||done<completion) {
            // Never destruct resources/code/events whose submitted ownership is
            // unproven. The failed process is not a normal-retirement receipt.
            for(auto& value:targets)(void)value.Detach();for(auto& value:readbacks)(void)value.Detach();
            (void)source.Detach();(void)upload.Detach();(void)commands.Detach();(void)allocator.Detach();(void)fence.Detach();
            (void)new std::shared_ptr<nr::interop::Context>(std::move(context));eventOwner.value=nullptr;
            std::printf("COPY_STORAGE_RETAINED signal=%ld wait=%lu completed=%llu credits=3 normal_release=false\n",
                signal,wait,static_cast<unsigned long long>(done));
            throw std::runtime_error("copy-only completion unresolved; ownership retained");
        }
        for(size_t slot=0;slot<3;++slot) {
            D3D12_RANGE range{0,static_cast<SIZE_T>(layout.physical_bytes())};mapped=nullptr;
            checked(readbacks[slot]->Map(0,&range,&mapped),"copy-only actual readback map");
            const auto rows=nr::qa::ghost_image_rows({static_cast<const uint8_t*>(mapped),static_cast<size_t>(layout.physical_bytes())},layout);
            for(size_t row=0;row<rows.rows;++row)expect(std::memcmp(rows.row(row).data(),
                logical.data()+row*rows.rowBytes,rows.rowBytes)==0,"actual pitched consumer lost storage bits/alpha/tail");
            if(rows.rowBytes==rows.rowPitch) {
                expect(std::memcmp(rows.tight().data(),logical.data(),logical.size())==0,"actual full CPU memcmp changed");
                if(format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_R16G16_FLOAT)
                    expect(nr::qa::bootstrap::finite_half(rows.tight())==nr::qa::bootstrap::finite_half(logical),
                        "actual complete half/alpha finite consumer changed");
            }else {
                expect(format==DXGI_FORMAT_R8_UINT,"copy-only unexpected padded native recipe");
                for(size_t row=0;row<rows.rows;++row)for(size_t x=0;x<rows.rowBytes;++x)
                    expect(rows.row(row)[x]==logical[row*rows.rowBytes+x],"actual pitched cost differs from old logical consumer");
                bool refused=false;try{(void)rows.tight();}catch(const std::runtime_error&){refused=true;}
                expect(refused,"actual padded cost exposed fake tight image");
            }
            readbacks[slot]->Unmap(0,&none);
            expect(credits.release(*tokens[slot],{completion,fence->GetCompletedValue(),completion,fence->GetCompletedValue(),
                true,true,true,true}),"copy-only map/consumer/fence credit retirement");
        }
        expect(credits.occupied()==0,"copy-only credits remained occupied");
        std::printf("COPY_STORAGE model_boundary=%u seed=%u format=%u logical=%llu physical_span=%llu row_bytes=%llu row_pitch=%u rows=%u target_count=3 target_GPU_capacity=%llu image_readback_count=3 image_READBACK_capacity=%llu source_textures=1 uploads=1 max_credits=3 compact_cost_capacity=0 maps_closed=true consumers_exact=true credits=0 shader_dispatch=0 model_inference=0 physical_acceptance=false\n",
            static_cast<unsigned>(modelBoundary),seed,static_cast<unsigned>(format),static_cast<unsigned long long>(layout.logical_bytes()),static_cast<unsigned long long>(layout.physical_bytes()),
            static_cast<unsigned long long>(layout.rowBytes),layout.footprint.Footprint.RowPitch,layout.rows,
            static_cast<unsigned long long>(targetCapacity),static_cast<unsigned long long>(readbackCapacity));
        for(auto& value:targets)value.Reset();for(auto& value:readbacks)value.Reset();source.Reset();upload.Reset();
        std::printf("COPY_STORAGE_RELEASE seed=%u format=%u images=0 uploads=0 readbacks=0 credits=0 normal_release=true\n",seed,static_cast<unsigned>(format));
    }
    commands.Reset();allocator.Reset();fence.Reset();context.reset();immediate.Reset();renderer.Reset();
    std::printf("COPY_STORAGE_CLOSED actual_logical_copy_bytes=%llu queues=0 contexts=0 images=0 maps=0 credits=0 normal_release=true shader_dispatch=0 model_inference=0 metadata_capacity=%zu\n",
        static_cast<unsigned long long>(copiedBytes),sizeof(nr::qa::GhostTextureBytes)+sizeof(nr::qa::GhostImageRows));
}
}
int nr_ghost_chunk_gpu_proof(const char*);
int main(int argc,char** argv) {
    try {
        expect(argc==2||(argc==3&&(std::string_view(argv[1])=="code-query"||
            std::string_view(argv[1])=="matched-code-before"||std::string_view(argv[1])=="matched-code-after"||
            std::string_view(argv[1])=="shader-proof")),
            "allocation/image proof selector");
        if(std::string_view(argv[1])=="cpu")cpu();
        else if(std::string_view(argv[1])=="copy-storage")copy_storage();
        else if(std::string_view(argv[1])=="copy-model-boundary")copy_storage(true);
        else if(std::string_view(argv[1])=="query-before")query(false);
        else if(std::string_view(argv[1])=="query-after")query(true);
        else if(std::string_view(argv[1])=="owners-before")query(false,true);
        else if(std::string_view(argv[1])=="owners-after")query(true,true);
        else if(std::string_view(argv[1])=="code-query")code_query(argv[2]);
        else if(std::string_view(argv[1])=="matched-code-before")matched_code_query(argv[2],false);
        else if(std::string_view(argv[1])=="matched-code-after")matched_code_query(argv[2],true);
        else if(std::string_view(argv[1])=="shader-proof")return nr_ghost_chunk_gpu_proof(argv[2]);
        else throw std::runtime_error("unknown allocation/image proof selector");
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL ghost_resource_admission %s checks=%u\n",e.what(),checks);return 1;}
}
