// SPDX-License-Identifier: GPL-3.0-only
#include "nr_flow_scenario.h"
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>
#ifdef NR_FLOW_WITH_OBS
#include "interop.h"
#include <obs.h>
#include <tlhelp32.h>
#endif
namespace nr::qa {
namespace {
using Microsoft::WRL::ComPtr;
void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
void checked(HRESULT h,const char* m) { require(SUCCEEDED(h),m); }
float half(uint16_t h) {
    // SDK vector decode and shader upsample are tested separately from half output readback.
    const int exponent=(h>>10)&31; const unsigned mantissa=h&1023;
    const float magnitude=exponent==31?(mantissa?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity()):
        (exponent?std::ldexp(float(1024+mantissa),exponent-25):std::ldexp(float(mantissa),-24));
    return h&32768?-magnitude:magnitude;
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; list->ResourceBarrier(1,&b);
}
std::string utf8(const std::wstring& text) {
    const int length=static_cast<int>(text.size());
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),length,nullptr,0,nullptr,nullptr);
    require(count>0,"flow module UTF8 size"); std::string result(static_cast<size_t>(count),char(0));
    require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),length,result.data(),count,nullptr,nullptr)==count,"flow module UTF8 conversion");
    return result;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* device,UINT64 size,D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=size;
    desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_UPLOAD?
        D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"QA buffer"); return result;
}
ComPtr<ID3D12Resource> analysis(ID3D12Device* device,Extent size) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=size.width; desc.Height=size.height;
    desc.DepthOrArraySize=1; desc.MipLevels=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1;
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result)),"QA BGRA analysis"); return result;
}
uint32_t fixture(int x,int y) {
    // Deterministic high-contrast multiscale texture, no wall-clock/random dependency.
    uint32_t h=static_cast<uint32_t>(x)*0x9e3779b9u ^ static_cast<uint32_t>(y)*0x85ebca6bu;
    h^=h<<13; h^=h>>17; h^=h<<5;
    const uint32_t tile=((static_cast<uint32_t>(x)/32)^(static_cast<uint32_t>(y)/32))&1;
    const uint32_t value=(h&63u)+(tile?160u:16u);
    return 0xff000000u | value | ((value^37u)<<8) | ((value^79u)<<16);
}
struct State {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    const Extent size;
    std::unique_ptr<D3D12Guides> guides;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> previous,current,uploadPrevious,uploadCurrent,readMotion,readDepth,readHardware,readCost,readCurrent,readPrevious;
    ComPtr<ID3D12Resource> readBootstrap,readGlobal,readHints;
    ComPtr<ID3D12Resource> readBootstrapCurrent,readBootstrapPrevious;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT pixels{},mvFootprint{},depthFootprint{},hardwareFootprint{},costFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT globalFootprint{},hintFootprint{};
    HANDLE event=nullptr;
    uint64_t serial=0;
    bool inFlight=false;
    State(ID3D12Device* d,ID3D12CommandQueue* q,Extent s,std::span<const std::byte> shader,uint64_t interval=0):device(d),queue(q),size(s) {
        // Cleanup is owned by State BEFORE the first asynchronous submit.
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"QA allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"QA list");
        checked(commands->Close(),"QA initial close");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"QA fence");
        previous=analysis(device,size); current=analysis(device,size);
        auto desc=previous->GetDesc(); UINT64 bytes=0;
        device->GetCopyableFootprints(&desc,0,1,0,&pixels,nullptr,nullptr,&bytes);
        uploadPrevious=buffer(device,bytes,D3D12_HEAP_TYPE_UPLOAD); uploadCurrent=buffer(device,bytes,D3D12_HEAP_TYPE_UPLOAD);
        readCurrent=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK); readPrevious=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        readBootstrapCurrent=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK); readBootstrapPrevious=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        desc.Format=DXGI_FORMAT_R16G16_FLOAT;
        device->GetCopyableFootprints(&desc,0,1,0,&mvFootprint,nullptr,nullptr,&bytes); readMotion=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        desc.Format=DXGI_FORMAT_R32_FLOAT;
        device->GetCopyableFootprints(&desc,0,1,0,&depthFootprint,nullptr,nullptr,&bytes); readDepth=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        guides=std::make_unique<D3D12Guides>(device,queue,size,shader,interval);
        desc.Width=(size.width+guides->grid_size()-1)/guides->grid_size();
        desc.Height=(size.height+guides->grid_size()-1)/guides->grid_size(); desc.Format=DXGI_FORMAT_R16G16_SINT;
        device->GetCopyableFootprints(&desc,0,1,0,&hardwareFootprint,nullptr,nullptr,&bytes); readHardware=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        readBootstrap=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        desc.Width=(size.width+guides->hint_grid_size()-1)/guides->hint_grid_size();
        desc.Height=(size.height+guides->hint_grid_size()-1)/guides->hint_grid_size();
        device->GetCopyableFootprints(&desc,0,1,0,&hintFootprint,nullptr,nullptr,&bytes); readHints=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        desc.Width=1; desc.Height=1;
        device->GetCopyableFootprints(&desc,0,1,0,&globalFootprint,nullptr,nullptr,&bytes); readGlobal=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        desc.Width=(size.width+guides->grid_size()-1)/guides->grid_size(); desc.Height=(size.height+guides->grid_size()-1)/guides->grid_size();
        desc.Format=guides->cost_format();
        device->GetCopyableFootprints(&desc,0,1,0,&costFootprint,nullptr,nullptr,&bytes); readCost=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK);
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"QA exact fence event");
    }
    ~State() { if(event) CloseHandle(event); }
    void begin() {
        require(!inFlight,"QA command reuse before physical completion");
        checked(allocator->Reset(),"QA reset allocator"); checked(commands->Reset(allocator.Get(),nullptr),"QA reset list");
    }
    void finish() {
        checked(commands->Close(),"QA close list");
        const auto next=++serial;
        // Subscribe exact fence completion BEFORE ExecuteCommandLists/Signal. No sleep/poll.
        checked(fence->SetEventOnCompletion(next,event),"QA subscribe completion"); inFlight=true;
        ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1,lists);
        checked(queue->Signal(fence.Get(),next),"QA completion signal");
        require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"QA unresolved physical completion; retain ownership");
        require(fence->GetCompletedValue()!=UINT64_MAX && fence->GetCompletedValue()>=next,"QA fence removed/incomplete");
        inFlight=false;
    }
    void upload(int dx,int dy);
    void copy_result(const FlowResult&);
    void verify(const FlowResult&,Rect,int dx,int dy,std::ostream&);
    bool close() {
        if(inFlight || guides->result()) return false;
        const auto physical=fence->GetCompletedValue();
        if(serial && (physical==UINT64_MAX || physical<serial)) return false;
        current.Reset();
        previous.Reset();
        return guides->close();
    }
};
void State::upload(int dx,int dy) {
    begin();
    const std::array<ID3D12Resource*,2> uploads{uploadPrevious.Get(),uploadCurrent.Get()};
    const std::array<ID3D12Resource*,2> images{previous.Get(),current.Get()};
    for(size_t index=0;index<2;++index) {
        std::byte* bytes=nullptr; D3D12_RANGE none{0,0};
        checked(uploads[index]->Map(0,&none,reinterpret_cast<void**>(&bytes)),"QA upload map");
        for(uint32_t y=0;y<size.height;++y) for(uint32_t x=0;x<size.width;++x) {
            const auto p=fixture(static_cast<int>(x)-(index?dx:0),static_cast<int>(y)-(index?dy:0));
            std::memcpy(bytes+pixels.Offset+size_t(y)*pixels.Footprint.RowPitch+size_t(x)*4,&p,4);
        }
        uploads[index]->Unmap(0,nullptr);
        transition(commands.Get(),images[index],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=uploads[index]; source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint=pixels;
        D3D12_TEXTURE_COPY_LOCATION dest{}; dest.pResource=images[index]; dest.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
        transition(commands.Get(),images[index],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    }
    finish();
}
void State::copy_result(const FlowResult& result) {
    begin();
    const std::array<ID3D12Resource*,11> sources{result.motion,result.depth,result.hardwareVectors,result.cost,current.Get(),previous.Get(),result.bootstrapVectors,result.globalVector,result.externalHints,result.bootstrapCurrent,result.bootstrapPrevious};
    const std::array<ID3D12Resource*,11> buffers{readMotion.Get(),readDepth.Get(),readHardware.Get(),readCost.Get(),readCurrent.Get(),readPrevious.Get(),readBootstrap.Get(),readGlobal.Get(),readHints.Get(),readBootstrapCurrent.Get(),readBootstrapPrevious.Get()};
    const std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,11> footprints{mvFootprint,depthFootprint,hardwareFootprint,costFootprint,pixels,pixels,hardwareFootprint,globalFootprint,hintFootprint,pixels,pixels};
    // Same owned queue orders after adapter conversion; explicit completion dependency also recorded.
    checked(queue->Wait(result.complete.fence,result.complete.value),"QA exact guide completion dependency");
    for(size_t i=0;i<(result.reset?2u:11u);++i) {
        transition(commands.Get(),sources[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=sources[i]; source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dest{}; dest.pResource=buffers[i]; dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dest.PlacedFootprint=footprints[i]; commands->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
        transition(commands.Get(),sources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
    finish();
}
void State::verify(const FlowResult& result,Rect active,int dx,int dy,std::ostream& evidence) {
    if(!result.reset) {
        unsigned inputErrors=0; uint64_t compared=0;
        std::array<uint64_t,4> inputHashes{}; inputHashes.fill(14695981039346656037ull);
        const std::array<ID3D12Resource*,4> inputs{readCurrent.Get(),readPrevious.Get(),readBootstrapCurrent.Get(),readBootstrapPrevious.Get()};
        for(size_t index=0;index<inputs.size();++index) {
            std::byte* bytes=nullptr; checked(inputs[index]->Map(0,nullptr,reinterpret_cast<void**>(&bytes)),"QA actual analysis readback");
            for(uint32_t y=0;y<size.height;++y) for(uint32_t x=0;x<size.width;++x) {
                uint32_t observed=0; std::memcpy(&observed,bytes+pixels.Offset+size_t(y)*pixels.Footprint.RowPitch+size_t(x)*4,4);
                const auto expected=fixture(static_cast<int>(x)-(index%2==0?dx:0),static_cast<int>(y)-(index%2==0?dy:0));
                if(observed!=expected) ++inputErrors;
                ++compared;
                for(unsigned b=0;b<4;++b) inputHashes[index]=(inputHashes[index]^((observed>>(b*8))&255u))*1099511628211ull;
            }
            inputs[index]->Unmap(0,nullptr);
        }
        evidence<<"NR_FLOW_INPUT_PROOF {\"current_sequence\":"<<result.key.frame.ingressSequence<<",\"previous_sequence\":"<<result.key.frame.ingressSequence-1
            <<",\"instance_id\":"<<result.key.frame.instanceId<<",\"history_epoch\":"<<result.key.historyEpoch<<",\"content_epoch\":"<<result.key.frame.contentEpoch
            <<",\"resource_generation\":"<<result.key.resourceGeneration<<",\"submission_serial\":"<<result.key.submissionSerial
            <<",\"format\":87,\"width\":"<<size.width<<",\"height\":"<<size.height<<",\"compared_pixels\":"<<compared
            <<",\"gpu_analysis_errors\":"<<inputErrors<<",\"input_hashes\":[";
        for(size_t i=0;i<inputHashes.size();++i) evidence<<(i?",":"")<<std::quoted(std::to_string(inputHashes[i]));
        evidence<<"]}"<<std::endl;
        require(inputErrors==0,"actual immutable GPU analysis differs from analytic current/reference");
        const std::array<ID3D12Resource*,9> identities{current.Get(),previous.Get(),result.bootstrapVectors,result.globalVector,result.externalHints,result.hardwareVectors,result.motion,result.bootstrapCurrent,result.bootstrapPrevious};
        for(size_t i=0;i<identities.size();++i) for(size_t j=i+1;j<identities.size();++j)
            require(identities[i] && identities[i]!=identities[j],"native pass allocation alias");
        const auto complete=[](NV_OF_FENCE_POINT point) {
            if(!point.fence || !point.value) return false;
            const auto actual=point.fence->GetCompletedValue(); return actual!=UINT64_MAX && actual>=point.value;
        };
        for(auto point:{result.currentProducer,result.previousProducer,result.inputAdmitted,result.bootstrapComplete,result.predictorComplete,result.refinementComplete,result.complete})
            require(complete(point),"native pass physical fence incomplete");
        require(result.inputAdmitted.fence==result.predictorComplete.fence && result.predictorComplete.fence==result.complete.fence &&
            result.inputAdmitted.value<result.predictorComplete.value && result.predictorComplete.value<result.complete.value &&
            result.bootstrapComplete.fence==result.refinementComplete.fence && result.bootstrapComplete.value<result.refinementComplete.value,
            "native pass fence chronology");
        std::byte* mapped=nullptr; int16_t globalValue[2]{};
        checked(readGlobal->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"QA global readback");
        std::memcpy(globalValue,mapped+globalFootprint.Offset,4); readGlobal->Unmap(0,nullptr);
        unsigned hintErrors=0,bootstrapErrors=0; float bootstrapMaximum=0;
        uint64_t hintHash=14695981039346656037ull,bootstrapHash=hintHash;
        checked(readHints->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"QA hint readback");
        for(uint32_t y=0;y<hintFootprint.Footprint.Height;++y) for(uint32_t x=0;x<hintFootprint.Footprint.Width;++x) {
            const auto* pixel=mapped+hintFootprint.Offset+size_t(y)*hintFootprint.Footprint.RowPitch+size_t(x)*4;
            const auto* nativePredictor=globalValue;
            if(std::memcmp(pixel,nativePredictor,4)) ++hintErrors;
            if(size.width==1920 && x==1332 && y==118) {
                int16_t source[2]{},hint[2]{}; std::memcpy(source,nativePredictor,4); std::memcpy(hint,pixel,4);
                evidence<<"NR_FLOW_HINT_TEXEL {\"x\":1332,\"y\":118,\"source_s10_5_x\":"<<source[0]<<",\"source_s10_5_y\":"<<source[1]
                    <<",\"hint_s10_5_x\":"<<hint[0]<<",\"hint_s10_5_y\":"<<hint[1]<<",\"hint_grid\":"<<guides->hint_grid_size()<<"}"<<std::endl;
            }
            for(unsigned b=0;b<4;++b) hintHash=(hintHash^std::to_integer<uint8_t>(pixel[b]))*1099511628211ull;
        }
        readHints->Unmap(0,nullptr);
        checked(readBootstrap->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"QA bootstrap readback");
        for(uint32_t y=0;y<hardwareFootprint.Footprint.Height;++y) for(uint32_t x=0;x<hardwareFootprint.Footprint.Width;++x) {
            const auto* pixel=mapped+hardwareFootprint.Offset+size_t(y)*hardwareFootprint.Footprint.RowPitch+size_t(x)*4;
            for(unsigned b=0;b<4;++b) bootstrapHash=(bootstrapHash^std::to_integer<uint8_t>(pixel[b]))*1099511628211ull;
            const uint32_t px=x*guides->grid_size(),py=y*guides->grid_size();
            if(px>=active.x+64 && py>=active.y+64 && px+64<active.x+active.width && py+64<active.y+active.height) {
                int16_t v[2]{}; std::memcpy(v,pixel,4); const auto decoded=decode_flow(v[0],v[1]);
                const auto error=std::max(std::abs(decoded.x+float(dx)),std::abs(decoded.y+float(dy)));
                if(error>1.f) ++bootstrapErrors; bootstrapMaximum=std::max(bootstrapMaximum,error);
            }
        }
        readBootstrap->Unmap(0,nullptr);
        evidence<<"NR_FLOW_REESTIMATION {\"sequence\":"<<result.key.frame.ingressSequence<<",\"instance_id\":"<<result.key.frame.instanceId
            <<",\"history_epoch\":"<<result.key.historyEpoch<<",\"content_epoch\":"<<result.key.frame.contentEpoch
            <<",\"submission_serial\":"<<result.key.submissionSerial<<",\"resource_generation\":"<<result.key.resourceGeneration
            <<",\"current_resource\":\""<<reinterpret_cast<uintptr_t>(current.Get())<<"\",\"reference_resource\":\""<<reinterpret_cast<uintptr_t>(previous.Get())
            <<"\",\"bootstrap_resource\":\""<<reinterpret_cast<uintptr_t>(result.bootstrapVectors)<<"\",\"global_resource\":\""<<reinterpret_cast<uintptr_t>(result.globalVector)
            <<"\",\"hint_resource\":\""<<reinterpret_cast<uintptr_t>(result.externalHints)<<"\",\"local_resource\":\""<<reinterpret_cast<uintptr_t>(result.hardwareVectors)
            <<"\",\"bootstrap_hash\":\""<<bootstrapHash<<"\",\"hint_hash\":\""<<hintHash<<"\",\"global_s10_5_x\":"<<globalValue[0]<<",\"global_s10_5_y\":"<<globalValue[1]
            <<",\"hint_errors\":"<<hintErrors<<",\"bootstrap_errors\":"<<bootstrapErrors<<",\"bootstrap_max_error\":"<<bootstrapMaximum
            <<",\"global_width\":"<<result.globalVector->GetDesc().Width<<",\"global_height\":"<<result.globalVector->GetDesc().Height
            <<",\"hint_grid\":"<<guides->hint_grid_size()<<",\"output_grid\":"<<guides->grid_size()
            <<",\"bootstrap_current_resource\":\""<<reinterpret_cast<uintptr_t>(result.bootstrapCurrent)<<"\",\"bootstrap_reference_resource\":\""<<reinterpret_cast<uintptr_t>(result.bootstrapPrevious)<<"\",\"registered_handles\":[";
        for(size_t i=0;i<result.registeredIdentities.size();++i) evidence<<(i?",":"")<<std::quoted(std::to_string(result.registeredIdentities[i]));
        evidence<<"],\"current_producer\":"<<result.currentProducer.value<<",\"previous_producer\":"<<result.previousProducer.value
            <<",\"producer_fence\":\""<<reinterpret_cast<uintptr_t>(result.currentProducer.fence)<<"\",\"ofa_fence\":\""<<reinterpret_cast<uintptr_t>(result.bootstrapComplete.fence)
            <<"\",\"worker_fence\":\""<<reinterpret_cast<uintptr_t>(result.complete.fence)<<"\",\"consumer_fence\":\""<<reinterpret_cast<uintptr_t>(fence.Get())<<"\""
            <<",\"input_admitted\":"<<result.inputAdmitted.value<<",\"bootstrap_ofa\":"<<result.bootstrapComplete.value<<",\"predictor_worker\":"<<result.predictorComplete.value
            <<",\"refinement_ofa\":"<<result.refinementComplete.value<<",\"conversion_worker\":"<<result.complete.value<<",\"consumer\":"<<serial
            <<",\"ofa_completed\":"<<result.refinementComplete.fence->GetCompletedValue()<<",\"worker_completed\":"<<result.complete.fence->GetCompletedValue()
            <<",\"consumer_completed\":"<<fence->GetCompletedValue()<<",\"native_passes\":2,\"predictor_source\":\"native_global\",\"hint_published_as_final_estimate\":false}"<<std::endl;
        require(hintErrors==0,"GPU predictor differs from unmodified native global output");
    }
    std::byte* mv=nullptr; std::byte* z=nullptr;
    checked(readMotion->Map(0,nullptr,reinterpret_cast<void**>(&mv)),"QA motion readback");
    const auto mappedDepth=readDepth->Map(0,nullptr,reinterpret_cast<void**>(&z));
    if(FAILED(mappedDepth)) { readMotion->Unmap(0,nullptr); checked(mappedDepth,"QA depth readback"); }
    std::byte* raw=nullptr; std::byte* costBytes=nullptr;
    if(!result.reset) {
        const auto a=readHardware->Map(0,nullptr,reinterpret_cast<void**>(&raw));
        if(FAILED(a)) { readMotion->Unmap(0,nullptr); readDepth->Unmap(0,nullptr); checked(a,"QA raw hardware readback"); }
        const auto b=readCost->Map(0,nullptr,reinterpret_cast<void**>(&costBytes));
        if(FAILED(b)) { readHardware->Unmap(0,nullptr); readMotion->Unmap(0,nullptr); readDepth->Unmap(0,nullptr); checked(b,"QA raw cost readback"); }
    }
    uint64_t hash=14695981039346656037ull,hardwareHash=hash;
    unsigned errors=0,scored=0; float maximum=0;
    for(uint32_t y=0;y<size.height;++y) for(uint32_t x=0;x<size.width;++x) {
        uint16_t components[2]{}; float neutral=0;
        const auto* pixel=mv+mvFootprint.Offset+size_t(y)*mvFootprint.Footprint.RowPitch+size_t(x)*4;
        std::memcpy(components,pixel,4);
        std::memcpy(&neutral,z+depthFootprint.Offset+size_t(y)*depthFootprint.Footprint.RowPitch+size_t(x)*4,4);
        for(unsigned i=0;i<4;++i) hash=(hash^std::to_integer<uint8_t>(pixel[i]))*1099511628211ull;
        if(!result.reset) {
            const auto* native=raw+hardwareFootprint.Offset+size_t(y/guides->grid_size())*hardwareFootprint.Footprint.RowPitch+size_t(x/guides->grid_size())*4;
            for(unsigned i=0;i<4;++i) hardwareHash=(hardwareHash^std::to_integer<uint8_t>(native[i]))*1099511628211ull;
        }
        if(neutral!=1.0f) ++errors;
        const FlowVector v{half(components[0]),half(components[1])};
        if(!result.reset && size.width==1920 && x==1332 && y==118) {
            int16_t native[2]{}; uint32_t rawCost=0;
            const auto gx=x/guides->grid_size(),gy=y/guides->grid_size();
            std::memcpy(native,raw+hardwareFootprint.Offset+size_t(gy)*hardwareFootprint.Footprint.RowPitch+size_t(gx)*4,4);
            const size_t stride=guides->cost_format()==DXGI_FORMAT_R8_UINT?1:4;
            std::memcpy(&rawCost,costBytes+costFootprint.Offset+size_t(gy)*costFootprint.Footprint.RowPitch+size_t(gx)*stride,stride);
            evidence<<"NR_FLOW_REFERENCE_TEXEL {\"sequence\":"<<result.key.frame.ingressSequence<<",\"x\":1332,\"y\":118,\"raw_x\":"<<native[0]
                <<",\"raw_y\":"<<native[1]<<",\"converted_x\":"<<v.x<<",\"converted_y\":"<<v.y<<",\"raw_cost\":"<<rawCost<<",\"cost_is_probability\":false}"<<std::endl;
        }
        const bool inside=x>=active.x && y>=active.y && x<active.x+active.width && y<active.y+active.height;
        if(result.reset) { if(v.x!=0 || v.y!=0) ++errors; }
        else if(!inside) { if(!std::isnan(v.x) || !std::isnan(v.y)) ++errors; }
        else if(x>=active.x+64 && y>=active.y+64 && x+64<active.x+active.width && y+64<active.y+active.height) {
            ++scored;
            const float error=std::max(std::abs(v.x+float(dx)),std::abs(v.y+float(dy)));
            if(!std::isfinite(error) || error>1.0f) {
                ++errors;
                if(errors<=5) {
                    const auto gx=x/guides->grid_size(),gy=y/guides->grid_size();
                    int16_t native[2]{}; uint32_t rawCost=0;
                    std::memcpy(native,raw+hardwareFootprint.Offset+size_t(gy)*hardwareFootprint.Footprint.RowPitch+size_t(gx)*4,4);
                    const size_t costStride=guides->cost_format()==DXGI_FORMAT_R8_UINT?1:4;
                    std::memcpy(&rawCost,costBytes+costFootprint.Offset+size_t(gy)*costFootprint.Footprint.RowPitch+size_t(gx)*costStride,costStride);
                    const auto decoded=decode_flow(native[0],native[1]);
                    std::array<uint32_t,9> neighborhood{}; size_t n=0;
                    for(int oy=-1;oy<=1;++oy) for(int ox=-1;ox<=1;++ox) {
                        const auto cx=static_cast<uint32_t>(static_cast<int>(gx)+ox),cy=static_cast<uint32_t>(static_cast<int>(gy)+oy);
                        std::memcpy(&neighborhood[n++],costBytes+costFootprint.Offset+size_t(cy)*costFootprint.Footprint.RowPitch+size_t(cx)*costStride,costStride);
                    }
                    std::sort(neighborhood.begin(),neighborhood.end());
                    evidence<<"NR_FLOW_VECTOR_ERROR {\"x\":"<<x<<",\"y\":"<<y<<",\"vx\":"<<v.x<<",\"vy\":"<<v.y
                        <<",\"hardware_s10_5_x\":"<<native[0]<<",\"hardware_s10_5_y\":"<<native[1]<<",\"hardware_decoded_x\":"<<decoded.x<<",\"hardware_decoded_y\":"<<decoded.y
                        <<",\"raw_cost\":"<<rawCost<<",\"neighbor_cost_min\":"<<neighborhood.front()<<",\"neighbor_cost_median\":"<<neighborhood[4]<<",\"neighbor_cost_max\":"<<neighborhood.back()
                        <<",\"cost_is_probability\":false,\"expected_x\":"<<-dx<<",\"expected_y\":"<<-dy<<",\"bound\":1}"<<std::endl;
                }
            }
            maximum=std::max(maximum,error);
        }
    }
    readMotion->Unmap(0,nullptr); readDepth->Unmap(0,nullptr);
    if(!result.reset) { readHardware->Unmap(0,nullptr); readCost->Unmap(0,nullptr); }
    evidence<<"NR_FLOW_PATTERN {\"scenario\":\"flow-translation\",\"width\":"<<size.width<<",\"height\":"<<size.height
        <<",\"sequence\":"<<result.key.frame.ingressSequence<<",\"history_epoch\":"<<result.key.historyEpoch
        <<",\"instance_id\":"<<result.key.frame.instanceId<<",\"content_epoch\":"<<result.key.frame.contentEpoch
        <<",\"resource_generation\":"<<result.key.resourceGeneration<<",\"submission_serial\":"<<result.key.submissionSerial
        <<",\"active_x\":"<<active.x<<",\"active_y\":"<<active.y<<",\"active_width\":"<<active.width<<",\"active_height\":"<<active.height
        <<",\"dx\":"<<dx<<",\"dy\":"<<dy<<",\"expected_mv_x\":"<<-dx<<",\"expected_mv_y\":"<<-dy
        <<",\"reset\":"<<(result.reset?"true":"false")<<",\"motion_hash\":\""<<hash<<"\""
        <<",\"hardware_hash\":\""<<(result.reset?0:hardwareHash)<<"\""
        <<",\"scored_pixels\":"<<scored<<",\"max_error_pixels\":"<<(std::isfinite(maximum)?maximum:-1.f)
        <<",\"fixed_vector_bound_pixels\":1,\"errors\":"<<errors<<"}"<<std::endl;
    require(result.reset || scored>0,"QA empty scored translation region");
    require(errors==0,"physical OFA vector/depth/reset/rectangle assertions failed; no tolerance widening");
}
} // private
int run_flow_translation(ID3D12Device* device,ID3D12CommandQueue* queue,std::span<const std::byte> shader,
    bool authorized,std::ostream& evidence) {
    if(!authorized) { evidence<<"{\"state\":\"CPU_READY_HARDWARE_NOT_AUTHORIZED\"}"<<std::endl; return 2; }
    if(!device || !queue || shader.empty()) { evidence<<"{\"state\":\"INVALID_OWNED_CONTEXT\"}"<<std::endl; return 1; }
    std::unique_ptr<State> state;
    try {
        for(const auto& pass:{std::pair{Extent{1920,1080},uint64_t{0}},std::pair{Extent{2560,1440},uint64_t{0}},
            std::pair{Extent{2560,1440},uint64_t{16666667}}}) { // third pass: owner 1440p@60 interval -> FAST, 4-pixel grid
            const Extent size=pass.first; const uint64_t interval=pass.second;
            state=std::make_unique<State>(device,queue,size,shader,interval);
            if(interval) require(state->guides->perf_level()==NV_OF_PERF_LEVEL_FAST && state->guides->grid_size()>=4,
                "owner 1440p@60 interval must select FAST with a 4-pixel or coarser grid");

            const auto& module=state->guides->module_identity();
            const auto& caps=state->guides->capabilities();
            evidence<<"NR_FLOW_CAPABILITY {\"module_path\":"<<std::quoted(utf8(module.path))
                <<",\"module_signer\":"<<std::quoted(utf8(module.signer))
                <<",\"module_sha256\":\""<<module.sha256<<"\",\"signed_nvidia_system32\":true,\"api_max\":"<<module.maxApi
                <<",\"grid\":"<<state->guides->grid_size()<<",\"cost_dxgi_format\":"<<static_cast<unsigned>(state->guides->cost_format())
                <<",\"submitted_perf_level\":"<<static_cast<unsigned>(state->guides->perf_level())<<",\"frame_interval_ns\":"<<interval
                <<",\"cost_is_probability\":false,\"supported_grids\":[";
            for(size_t i=0;i<caps.grids.size();++i) evidence<<(i?",":"")<<caps.grids[i];
            evidence<<"],\"input_formats\":[";
            for(size_t i=0;i<caps.inputFormats.size();++i) evidence<<(i?",":"")<<static_cast<unsigned>(caps.inputFormats[i]);
            evidence<<"],\"output_formats\":[";
            for(size_t i=0;i<caps.outputFormats.size();++i) evidence<<(i?",":"")<<static_cast<unsigned>(caps.outputFormats[i]);
            evidence<<"],\"cost_formats\":[";
            for(size_t i=0;i<caps.costFormats.size();++i) evidence<<(i?",":"")<<static_cast<unsigned>(caps.costFormats[i]);
            evidence<<"],\"hint_support\":[";
            for(size_t i=0;i<caps.hintSupport.size();++i) evidence<<(i?",":"")<<caps.hintSupport[i];
            evidence<<"],\"hint_grids\":[";
            for(size_t i=0;i<caps.hintGrids.size();++i) evidence<<(i?",":"")<<caps.hintGrids[i];
            evidence<<"],\"hint_formats\":[";
            for(size_t i=0;i<caps.hintFormats.size();++i) evidence<<(i?",":"")<<static_cast<unsigned>(caps.hintFormats[i]);
            evidence<<"],\"global_formats\":[";
            for(size_t i=0;i<caps.globalFormats.size();++i) evidence<<(i?",":"")<<static_cast<unsigned>(caps.globalFormats[i]);
            evidence<<"]}"<<std::endl;
            uint64_t sequence=0,epoch=0;
            const uint64_t instance=size.width==1920?81u:(interval?83u:82u);
            for(const auto active:{Rect{0,0,size.width,size.height},Rect{16,24,size.width-32,size.height-48}})
                for(auto amount:{1,8,32}) for(auto sign:{-1,1}) for(auto axis:{0,1}) {
                    const int dx=axis==0?amount*sign:0,dy=axis==1?amount*sign:0;
                    ++epoch;

                    state->upload(dx,dy);

                    const JobKey referenceKey{{instance,epoch,++sequence},epoch,1,sequence};
                    const FlowImage reference{referenceKey,state->previous.Get(),{state->fence.Get(),state->serial}};
                    const FlowInput cold{reference,nullptr,size,active,true};
                    state->inFlight=true;

                    require(state->guides->submit(cold)==FlowProblem::None,"physical OFA cold admission");
                    state->inFlight=false; // Adapter independently retains submitted ownership.
                    const auto* reset=state->guides->result();
                    require(reset && reset->reset && !reset->cost && !reset->hardwareVectors && !reset->bootstrapVectors &&
                        !reset->globalVector && !reset->externalHints && !reset->bootstrapCurrent && !reset->bootstrapPrevious &&
                        !reset->bootstrapComplete.fence && !reset->predictorComplete.fence && !reset->refinementComplete.fence,
                        "reset cannot expose stale native predictor/pass output");

                    state->copy_result(*reset); state->verify(*reset,active,0,0,evidence);

                    require(state->guides->collect(referenceKey,{state->fence.Get(),state->serial}),"physical cold ownership collection");
                    const JobKey currentKey{{instance,epoch,++sequence},epoch,1,sequence};
                    const FlowImage current{currentKey,state->current.Get(),{state->fence.Get(),state->serial}};
                    const FlowInput ordinary{current,&reference,size,active,false};
                    const JobKey staleKey{{instance,epoch,sequence-1},epoch+1,1,sequence-1};
                    const FlowImage stale{staleKey,state->previous.Get(),reference.ready};
                    require(state->guides->submit({current,&stale,size,active,false})==FlowProblem::KeyMismatch,"old guide epoch accepted");
                    state->inFlight=true;

                    require(state->guides->submit(ordinary)==FlowProblem::None,"physical OFA translation admission");
                    state->inFlight=false;
                    require(state->guides->submit(ordinary)==FlowProblem::Busy,"in-flight guide resource overwritten");
                    const auto* result=state->guides->result();
                    require(result && result->key==currentKey && !result->reset && result->cost &&
                        result->cost->GetDesc().Format==state->guides->cost_format(),"actual cost/result format identity");
                    state->copy_result(*result); state->verify(*result,active,dx,dy,evidence);
                    require(!state->guides->collect(staleKey,{state->fence.Get(),state->serial}),"old key collected live resource");
                    require(state->guides->collect(currentKey,{state->fence.Get(),state->serial}),"physical guide ownership collection");
                }
            require(state->close(),"physical OFA session cleanup incomplete"); state.reset();
        }
        evidence<<"{\"scenario\":\"flow-translation\",\"state\":\"PASS\",\"evidence_class\":\"physical_ofa_only_not_Feature18\",\"cleanup\":\"owned_completed\"}"<<std::endl;
        evidence<<"NR_FLOW_SCOPE physical_ofa=true model=not_loaded Feature18=not_admitted HDR=not_claimed recording=not_tested cleanup=complete"<<std::endl;
        return 0;
    } catch(const std::exception& error) {
        bool retained=false;
        if(state && !state->inFlight && state->guides->result()) {
            const auto* ready=state->guides->result();
            if(!state->guides->collect(ready->key,{state->fence.Get(),state->serial}))
                evidence<<"NR_FLOW_COLLECTION_NOT_PROVEN"<<std::endl;
        }
        if(state && (state->inFlight || !state->close())) { (void)state.release(); retained=true; }
        evidence<<"{\"scenario\":\"flow-translation\",\"state\":\"FAIL\",\"cleanup\":\""<<(retained?"unresolved_owned_resources_retained":"owned_completed")<<"\"}"<<std::endl;
        evidence<<"FAIL flow-translation: "<<error.what()<<std::endl;
        return retained?2:1;
    }
}
} // nr::qa
#ifdef NR_FLOW_WITH_OBS
namespace {
void flow_refuse_unrelated() {
    const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE) throw std::runtime_error("flow process admission snapshot failed");
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry); bool rejected=false; DWORD other=0;
    if(!Process32FirstW(snapshot,&entry)) { CloseHandle(snapshot); throw std::runtime_error("flow process admission enumeration failed"); }
    do {
        if(entry.th32ProcessID==GetCurrentProcessId()) continue;
        for(const auto name:{L"obs64.exe",L"nr-qa-host.exe",L"CrimsonDesert.exe",L"GTA5.exe",L"GTA5_Enhanced.exe",L"Cyberpunk2077.exe",
            L"eldenring.exe",L"BlackMythWukong.exe",L"cs2.exe",L"dota2.exe"})
            if(_wcsicmp(entry.szExeFile,name)==0) { rejected=true; other=entry.th32ProcessID; }
        if(_wcsnicmp(entry.szExeFile,L"FortniteClient",14)==0) { rejected=true; other=entry.th32ProcessID; }
    } while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    if(rejected) throw std::runtime_error("flow unrelated active process refused PID="+std::to_string(other));
}
}
bool nr_run_flow_scenario(const std::filesystem::path& root) {
    std::cout<<"NR_FLOW_CLEANUP_REGISTERED owned_context_queues_events_resources_only"<<std::endl;
    std::shared_ptr<nr::interop::Context> context;
    gs_texture_t* fixture=nullptr;
    bool graphicsEntered=false;
    try {
        const auto path=root/"bin/64bit/nr_guides.cso";
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file) throw std::runtime_error("flow shader missing");
        const auto count=file.tellg(); if(count<=0) throw std::runtime_error("flow shader empty");
        std::vector<std::byte> bytes(static_cast<size_t>(count)); file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()),count); if(!file) throw std::runtime_error("flow shader read");
        flow_refuse_unrelated(); // Immediate hardware admission, excluding only THIS owned host PID.
        obs_enter_graphics(); graphicsEntered=true;
        if(gs_get_device_type()!=GS_DEVICE_DIRECT3D_11) throw std::runtime_error("flow requires admitted renderer D3D11");
        fixture=gs_texture_create(16,16,GS_BGRA,1,nullptr,GS_DYNAMIC);
        if(!fixture) throw std::runtime_error("flow renderer identity fixture");
        const auto borrowed=static_cast<ID3D11Texture2D*>(gs_texture_get_obj(fixture));
        if(!borrowed) throw std::runtime_error("flow renderer object identity");
        Microsoft::WRL::ComPtr<ID3D11Device> renderer; borrowed->GetDevice(&renderer);
        context=nr::interop::Context::create(renderer.Get());
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory; Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
           FAILED(factory->EnumAdapterByLuid(context->luid(),IID_PPV_ARGS(&adapter)))) throw std::runtime_error("flow adapter identity");
        DXGI_ADAPTER_DESC1 desc{}; if(FAILED(adapter->GetDesc1(&desc))) throw std::runtime_error("flow adapter description");
        if(desc.VendorId!=0x10de || std::wstring(desc.Description).find(L"RTX 4090")==std::wstring::npos) throw std::runtime_error("flow requires actual admitted RTX4090");
        const auto worker=context->device()->GetAdapterLuid();
        if(!nr::interop::same_luid(worker,context->luid())) throw std::runtime_error("flow private same-LUID proof failed");
        std::cout<<"NR_FLOW_CONTEXT {\"renderer_luid_high\":"<<context->luid().HighPart<<",\"renderer_luid_low\":"<<context->luid().LowPart
            <<",\"worker_luid_high\":"<<worker.HighPart<<",\"worker_luid_low\":"<<worker.LowPart
            <<",\"vendor_id\":"<<desc.VendorId<<",\"device_id\":"<<desc.DeviceId<<",\"adapter\":\"RTX 4090\",\"private_direct_queue\":true}"<<std::endl;
        gs_texture_destroy(fixture); fixture=nullptr;
        obs_leave_graphics(); graphicsEntered=false;
        // Finite QA work runs OUTSIDE the OBS graphics lock. No worker renderer-context calls.
        const int result=nr::qa::run_flow_translation(context->device(),context->queue(),bytes,true,std::cout);
        if(result==2) {
            (void)new std::shared_ptr<nr::interop::Context>(std::move(context));
            std::cout<<"NR_FLOW_RETAINED unresolved=true"<<std::endl;
        }
        return result==0;
    } catch(const std::exception& error) {
        if(fixture) gs_texture_destroy(fixture);
        if(graphicsEntered) obs_leave_graphics();
        std::cout<<"NR_FLOW_FAILURE "<<error.what()<<std::endl; return false;
    }
}
#endif
