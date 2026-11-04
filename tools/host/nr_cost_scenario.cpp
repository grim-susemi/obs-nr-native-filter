// SPDX-License-Identifier: GPL-3.0-only
#include "nr_cost_scenario.h"
#include "nr_cost_driver.h"
#include "nr_cost_completion.h"
#include "nr_model_ownership.h"
#include "nr_core.h"
#include "interop.h"
#include <obs.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace nr;
using Microsoft::WRL::ComPtr;
void check(bool ok,const char* what) { if(!ok) throw std::runtime_error(what); }
void check(HRESULT hr,const char* what) { check(SUCCEEDED(hr),what); }
struct Event {
    HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Event() { check(value!=nullptr,"cost event"); }
    ~Event() { CloseHandle(value); }
};
struct GraphicsScope {
    GraphicsScope() { obs_enter_graphics(); }
    ~GraphicsScope() { obs_leave_graphics(); }
};
struct Texture {
    gs_texture_t* value=gs_texture_create(4,4,GS_BGRA,1,nullptr,GS_DYNAMIC);
    ~Texture() { gs_texture_destroy(value); }
};
struct Work {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::shared_ptr<runtime::Submission> submission=std::make_shared<runtime::Submission>();
    Event event;
    bool outstanding=false;
    explicit Work(ID3D12Device* input):device(input) {
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"cost allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"cost list");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"cost fence");
        check(fence->SetEventOnCompletion(1,event.value),"cost completion subscription");
    }
    runtime::Recording recording(uint64_t epoch,const std::shared_ptr<Work>& lease) {
        auto s=submission;
        return {list.Get(),epoch,lease,[s]{return s->state();}};
    }
    double submit(ID3D12CommandQueue* queue,qa::CostStage stage,unsigned frame) {
        check(list->Close(),"cost close list");
        LARGE_INTEGER start{},end{},freq{}; QueryPerformanceFrequency(&freq);
        ID3D12CommandList* lists[]{list.Get()};
        QueryPerformanceCounter(&start);
        outstanding=true; queue->ExecuteCommandLists(1,lists);
        const bool accepted=submission->submitted(queue);
        std::optional<HRESULT> signal;
        std::optional<DWORD> wait,waitError;
        if(accepted) {
            signal=queue->Signal(fence.Get(),1);
            if(SUCCEEDED(*signal)) {
                const auto result=WaitForSingleObject(event.value,30000);
                if(result==WAIT_FAILED) waitError=GetLastError();
                wait=result;
            }
        }
        qa::CostCompletionFacts facts{stage,frame,accepted,signal,wait,waitError,
            fence->GetCompletedValue(),submission->state(),S_OK};
        if(!qa::cost_completion_confirmed(facts)) {
            facts.deviceRemoved=device->GetDeviceRemovedReason();
            qa::write_cost_completion(std::cout,facts);
            throw std::runtime_error("cost exact physical completion");
        }
        QueryPerformanceCounter(&end); outstanding=false;
        return 1000.0*static_cast<double>(end.QuadPart-start.QuadPart)/static_cast<double>(freq.QuadPart);
    }
};
struct Resources {
    std::array<ComPtr<ID3D12Resource>,4> image;
    ComPtr<ID3D12DescriptorHeap> views;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
};
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* image,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={image,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; list->ResourceBarrier(1,&b);
}
std::shared_ptr<Resources> resources(ID3D12Device* device,Extent size,ID3D12GraphicsCommandList* list) {
    auto r=std::make_shared<Resources>();
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=4; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&r->views)),"cost descriptors");
    D3D12_QUERY_HEAP_DESC q{}; q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; q.Count=2;
    check(device->CreateQueryHeap(&q,IID_PPV_ARGS(&r->queries)),"cost timestamp heap");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    const DXGI_FORMAT formats[]{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
    auto cpu=r->views->GetCPUDescriptorHandleForHeapStart();
    auto gpu=r->views->GetGPUDescriptorHandleForHeapStart();
    const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap* heaps[]{r->views.Get()}; list->SetDescriptorHeaps(1,heaps);
    for(size_t i=0;i<r->image.size();++i) {
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=size.width; d.Height=size.height; d.DepthOrArraySize=d.MipLevels=1;
        d.Format=formats[i]; d.SampleDesc.Count=1; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,IID_PPV_ARGS(&r->image[i])),"cost texture");
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=formats[i]; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r->image[i].Get(),nullptr,&view,cpu);
        const float value[]{i==1?1.0f:0.25f,0,0,1};
        list->ClearUnorderedAccessViewFloat(gpu,cpu,r->image[i].Get(),value,0,nullptr);
        if(i!=3) transition(list,r->image[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        else {
            D3D12_RESOURCE_BARRIER uav{}; uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource=r->image[i].Get();
            list->ResourceBarrier(1,&uav);
        }
        cpu.ptr+=stride; gpu.ptr+=stride;
    }
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC b{}; b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; b.Width=16;
    b.Height=1; b.DepthOrArraySize=b.MipLevels=1; b.SampleDesc.Count=1; b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&r->readback)),"cost query readback");
    return r;
}
struct Owner {
    std::shared_ptr<interop::Context> context;
    std::shared_ptr<qa::CostDriver> driver;
    std::shared_ptr<runtime::Module> module;
    std::unique_ptr<runtime::Pass> pass;
    std::shared_ptr<Resources> images;
    std::shared_ptr<Work> work;
};
}

bool nr_run_cost_scenario(const std::filesystem::path& root,std::wstring_view text,bool& retained) {
    const std::array<std::pair<std::wstring_view,Extent>,6> sizes{{
        {L"2560x1440",{2560,1440}},{L"1920x1080",{1920,1080}},{L"1280x720",{1280,720}},
        {L"959x539",{959,539}},{L"480x270",{480,270}},{L"64x64",{64,64}}}};
    const auto found=std::find_if(sizes.begin(),sizes.end(),[text](const auto& x){return x.first==text;});
    if(found==sizes.end()) { std::cout<<"NR_COST_INVALID_SIZE\n"; return false; }
    const auto size=found->second;
    auto owner=std::make_unique<Owner>();
    try {
        {
            GraphicsScope graphics;
            Texture texture;
            check(texture.value!=nullptr,"cost renderer texture");
            auto* borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(texture.value));
            ComPtr<ID3D11Device> renderer; if(borrowed) borrowed->GetDevice(&renderer);
            if(renderer) owner->context=interop::Context::create(renderer.Get());
        }
        check(owner->context!=nullptr,"cost renderer context");
        auto* device=owner->context->device(); auto* queue=owner->context->queue();
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 desc{};
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"cost DXGI factory");
        check(factory->EnumAdapterByLuid(owner->context->luid(),IID_PPV_ARGS(&adapter)),"cost adapter LUID");
        check(adapter->GetDesc1(&desc),"cost adapter description");
        check(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) &&
            std::wstring(desc.Description).find(L"RTX 4090")!=std::wstring::npos &&
            interop::same_luid(owner->context->luid(),device->GetAdapterLuid()),"cost adapter admission");
        UINT64 frequency=0; check(queue->GetTimestampFrequency(&frequency),"cost timestamp frequency");
        check(frequency!=0,"cost zero timestamp frequency");
        const auto data=std::filesystem::canonical(root/"profile/ngx-data");
        qa::open_cost_driver(owner->driver,device,data);
        const runtime::RuntimeIdentity identity{std::filesystem::canonical(L"C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll"),
            "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a"};
        owner->module=runtime::Module::open(identity,qa::cost_driver_dispatch(owner->driver),owner->driver,
            device,data,runtime::Route::DriverFallback);
        check(owner->module!=nullptr,"cost runtime module");
        owner->pass=std::make_unique<runtime::Pass>(owner->module);
        const Settings settings{{1},true,identity.canonicalPath.generic_string(),{1},core_default_tuning()};
        owner->work=std::make_shared<Work>(device);
        owner->images=resources(device,size,owner->work->list.Get());
        const auto creation=owner->pass->prepare(settings,size,1,owner->work->recording(1,owner->work));
        std::cout<<"NR_COST_PREPARE_RETURN {\"result\":"<<static_cast<uint32_t>(creation)<<"}"<<std::endl;
        const double creationLatency=owner->work->submit(queue,qa::CostStage::Prepare,0);
        std::cout<<"NR_COST_CREATE {\"width\":"<<size.width<<",\"height\":"<<size.height
            <<",\"result\":"<<static_cast<uint32_t>(creation)<<",\"probe_submission_qpc_ms\":"<<creationLatency<<"}"<<std::endl;
        if(creation==NVSDK_NGX_Result_Success) {
            check(owner->pass->ready(2),"cost creation ready");
            HMODULE image=nullptr;
            check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,identity.canonicalPath.c_str(),&image)!=FALSE &&
                image!=nullptr,"cost selected runtime not loaded");
            std::cout<<"NR_COST_MODULE {\"path\":\""<<identity.canonicalPath.generic_string()<<"\",\"sha256\":\""
                <<identity.sha256<<"\",\"base\":\""<<reinterpret_cast<uintptr_t>(image)<<"\"}"<<std::endl;
            for(uint64_t frame=1;frame<=6;++frame) {
                owner->work=std::make_shared<Work>(device);
                const JobContract job{{{1,1,frame},1,1,frame},{{1,1,frame},frame+1,frame,1,1,1,
                    SourceValidity::Valid,{{size.width,size.height},{0,0,size.width,size.height},PixelFormat::Bgra8,
                    ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},settings},frame+2};
                const Rect region{0,0,size.width,size.height};
                const runtime::Inputs input{owner->images->image[0].Get(),owner->images->image[1].Get(),
                    owner->images->image[2].Get(),owner->images->image[3].Get(),region,region,false,frame==1,1,1,true};
                auto* list=owner->work->list.Get();
                D3D12_RESOURCE_BARRIER uav{}; uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uav.UAV.pResource=owner->images->image[3].Get(); list->ResourceBarrier(1,&uav);
                list->EndQuery(owner->images->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
                const auto result=owner->pass->run(job,input,owner->work->recording(frame+1,owner->work));
                std::cout<<"NR_COST_PASS_RETURN {\"frame\":"<<frame<<",\"result\":"
                    <<static_cast<uint32_t>(result)<<"}"<<std::endl;
                list->EndQuery(owner->images->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
                list->ResolveQueryData(owner->images->queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,owner->images->readback.Get(),0);
                const auto latency=owner->work->submit(queue,qa::CostStage::Evaluation,static_cast<unsigned>(frame));
                if(result!=NVSDK_NGX_Result_Success) {
                    std::cout<<"NR_COST_EVALUATE_FAILURE {\"frame\":"<<frame<<",\"result\":"<<static_cast<uint32_t>(result)<<"}"<<std::endl;
                    break;
                }
                const auto complete=owner->pass->completed(job.key);
                check(complete && complete->evaluated && complete->workerComplete,"cost evaluation completion");
                void* bytes=nullptr; const D3D12_RANGE range{0,16};
                check(owner->images->readback->Map(0,&range,&bytes),"cost query map");
                const auto* stamps=static_cast<const uint64_t*>(bytes);
                const auto begin=stamps[0],end=stamps[1]; const D3D12_RANGE empty{0,0};
                owner->images->readback->Unmap(0,&empty);
                check(end>begin,"cost invalid timestamps");
                const double gpuMs=1000.0*static_cast<double>(end-begin)/static_cast<double>(frequency);
                std::cout<<"NR_COST_FRAME {\"frame\":"<<frame<<",\"reset\":"<<(frame==1?"true":"false")
                    <<",\"result\":"<<static_cast<uint32_t>(result)<<",\"begin\":"<<begin<<",\"end\":"<<end
                    <<",\"frequency\":"<<frequency<<",\"gpu_ms\":"<<gpuMs<<",\"probe_submission_qpc_ms\":"<<latency<<"}"<<std::endl;
            }
        }
        check(owner->pass->release(),"cost pass release");
        owner->pass.reset();
        check(owner->module->close(),"cost module close"); owner->module.reset();
        check(qa::close_cost_driver(owner->driver),"cost driver close"); owner->driver.reset();
        std::cout<<"NR_COST_CLEANUP complete=true"<<std::endl;
        return true;
    } catch(const std::exception& e) {
        std::cout<<"NR_COST_FAILURE "<<e.what()<<std::endl;
        bool safe=!(owner->work && owner->work->outstanding);
        if(safe && owner->work && owner->work->submission->state()==runtime::Completion::Recorded)
            safe=owner->work->submission->discard();
        if(safe && owner->pass) { safe=owner->pass->release(); if(safe) owner->pass.reset(); }
        if(safe && owner->module) { safe=owner->module->close(); if(safe) owner->module.reset(); }
        if(safe && owner->driver) { safe=qa::close_cost_driver(owner->driver); if(safe) owner->driver.reset(); }
        if(!safe) { retained=true; qa::retain_unresolved_model(owner); }
        return false;
    }
}
