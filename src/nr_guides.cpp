// SPDX-License-Identifier: GPL-3.0-only
#include "nr_guides.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <wrl/client.h>

namespace nr {
FlowVector decode_flow(int16_t x,int16_t y) { return {float(x)/32.f,float(y)/32.f}; }
std::optional<uint32_t> smallest_flow_grid(std::span<const uint32_t> caps) {
    if(caps.empty()) return {};
    uint32_t result=4;
    for(auto grid:caps) { if(grid!=1 && grid!=2 && grid!=4) return {}; result=std::min(result,grid); }
    return result;
}
bool flow_geometry_valid(Extent size,Rect r) {
    return size.width && size.height && r.width && r.height && r.x<=size.width && r.y<=size.height &&
        r.width<=size.width-r.x && r.height<=size.height-r.y;
}
bool flow_cost_format_valid(DXGI_FORMAT f) { return f==DXGI_FORMAT_R8_UINT || f==DXGI_FORMAT_R32_UINT; }
FlowProblem flow_pair_problem(const JobKey& c,const JobKey* p,bool reset) {
    if(!c.valid()) return FlowProblem::InvalidKey;
    if(reset) return p?FlowProblem::ResetPair:FlowProblem::None;
    if(!p) return FlowProblem::MissingPrevious;
    if(!p->valid() || c.frame.instanceId!=p->frame.instanceId || c.frame.contentEpoch!=p->frame.contentEpoch ||
       c.historyEpoch!=p->historyEpoch || c.resourceGeneration!=p->resourceGeneration ||
       p->frame.ingressSequence>=c.frame.ingressSequence || c.frame.ingressSequence-p->frame.ingressSequence!=1 ||
       p->submissionSerial>=c.submissionSerial) return FlowProblem::KeyMismatch;
    return FlowProblem::None;
}
FlowProblem flow_chronology_problem(const JobKey& current,const JobKey* previous,const JobKey* last,
    Rect active,const Rect* lastActive,bool reset) {
    const auto pair=flow_pair_problem(current,previous,reset);
    if(pair!=FlowProblem::None) return pair;
    if(!last) return reset?FlowProblem::None:FlowProblem::MissingPrevious;
    if(current.frame.instanceId!=last->frame.instanceId || current.resourceGeneration!=last->resourceGeneration ||
       current.submissionSerial<=last->submissionSerial || current.frame.ingressSequence<=last->frame.ingressSequence)
        return FlowProblem::KeyMismatch;
    if(reset) return current.historyEpoch>last->historyEpoch?FlowProblem::None:FlowProblem::ResetPair;
    return previous && *previous==*last && lastActive && active==*lastActive?FlowProblem::None:FlowProblem::KeyMismatch;
}
NV_OF_INIT_PARAMS flow_init_params(Extent size,uint32_t grid) {
    NV_OF_INIT_PARAMS p{}; p.width=size.width; p.height=size.height;
    p.outGridSize=static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid);
    p.mode=NV_OF_MODE_OPTICALFLOW; p.perfLevel=NV_OF_PERF_LEVEL_MEDIUM;
    p.enableOutputCost=NV_OF_TRUE; p.predDirection=NV_OF_PRED_DIRECTION_FORWARD;
    p.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8; return p;
}
std::optional<uint32_t> smallest_hint_grid(std::span<const uint32_t> caps,uint32_t outputGrid) {
    std::optional<uint32_t> result;
    for(auto grid:caps) {
        if(grid!=1 && grid!=2 && grid!=4 && grid!=8) return {};
        if(grid>=outputGrid && (!result || grid<*result)) result=grid;
    }
    return result;
}
NV_OF_INIT_PARAMS flow_bootstrap_params(Extent size,uint32_t grid) {
    auto p=flow_init_params(size,grid); p.enableGlobalFlow=NV_OF_TRUE; return p;
}
NV_OF_INIT_PARAMS flow_refinement_params(Extent size,uint32_t grid,uint32_t hintGrid) {
    auto p=flow_init_params(size,grid); p.enableExternalHints=NV_OF_TRUE;
    p.hintGridSize=static_cast<NV_OF_HINT_VECTOR_GRID_SIZE>(hintGrid); return p;
}
NV_OF_EXECUTE_INPUT_PARAMS_D3D12 flow_execute_input(NvOFGPUBufferHandle c,NvOFGPUBufferHandle p,bool reset,
    std::span<NV_OF_FENCE_POINT> dependencies,NvOFGPUBufferHandle hint) {
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{}; in.inputFrame=c; in.referenceFrame=p;
    in.externalHints=hint; in.disableTemporalHints=reset?NV_OF_TRUE:NV_OF_FALSE;
    in.numFencePoints=static_cast<uint32_t>(dependencies.size()); in.fencePoint=dependencies.data(); return in;
}
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT h,const char* message) { if(FAILED(h)) throw std::runtime_error(message); }
void of_checked(NV_OF_STATUS s,const char* message) {
    if(s!=NV_OF_SUCCESS) throw std::runtime_error(std::string(message)+" status="+std::to_string(static_cast<unsigned>(s)));
}
bool completed(NV_OF_FENCE_POINT p) {
    if(!p.fence || !p.value || p.value==UINT64_MAX) return false;
    const auto actual=p.fence->GetCompletedValue(); return actual!=UINT64_MAX && actual>=p.value;
}

struct DriverModule {
    HMODULE module=nullptr;
    FlowModuleIdentity identity;
    NV_OF_D3D12_API_FUNCTION_LIST api{};
    ~DriverModule() { if(module) FreeLibrary(module); }
    void load();
};
void DriverModule::load() {
    std::array<wchar_t,32768> directory{};
    const UINT length=GetSystemDirectoryW(directory.data(),static_cast<UINT>(directory.size()));
    if(!length || length>=directory.size()) throw std::runtime_error("System32 unavailable");
    identity.path=std::wstring(directory.data())+wchar_t(92)+L"nvofapi64.dll";
    HANDLE file=CreateFileW(identity.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("System32 OFA module unavailable");
    try {
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(file,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
           info.nFileSizeHigh || !info.nFileSizeLow || info.nFileSizeLow>64u*1024u*1024u)
            throw std::runtime_error("OFA module file admission");
        std::vector<BYTE> bytes(info.nFileSizeLow); DWORD read=0;
        if(!ReadFile(file,bytes.data(),info.nFileSizeLow,&read,nullptr) || read!=info.nFileSizeLow)
            throw std::runtime_error("OFA module hash read");
        std::array<BYTE,32> digest{}; DWORD count=static_cast<DWORD>(digest.size());
        if(!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM,0,nullptr,bytes.data(),read,digest.data(),&count))
            throw std::runtime_error("OFA module hash");
        const char hex[]="0123456789abcdef";
        for(auto b:digest) { identity.sha256+=hex[b>>4]; identity.sha256+=hex[b&15]; }
        WINTRUST_FILE_INFO fi{}; fi.cbStruct=sizeof(fi); fi.pcwszFilePath=identity.path.c_str(); fi.hFile=file;
        WINTRUST_DATA data{}; data.cbStruct=sizeof(data); data.dwUIChoice=WTD_UI_NONE;
        data.fdwRevocationChecks=WTD_REVOKE_NONE; data.dwUnionChoice=WTD_CHOICE_FILE; data.pFile=&fi;
        data.dwStateAction=WTD_STATEACTION_VERIFY; data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
        GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG trust=WinVerifyTrust(nullptr,&action,&data);
        const auto provider=WTHelperProvDataFromStateData(data.hWVTStateData);
        const auto signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        if(signer && signer->csCertChain && signer->pasCertChain[0].pCert) {
            std::array<wchar_t,512> name{};
            CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,
                               name.data(),static_cast<DWORD>(name.size()));
            identity.signer=name.data();
        }
        data.dwStateAction=WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr,&action,&data);
        if(trust!=ERROR_SUCCESS || identity.signer!=L"NVIDIA Corporation")
            throw std::runtime_error("OFA module NVIDIA signature rejected");
        module=LoadLibraryExW(identity.path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module) throw std::runtime_error("OFA signed module load");
        std::array<wchar_t,32768> loaded{};
        if(!GetModuleFileNameW(module,loaded.data(),static_cast<DWORD>(loaded.size())) ||
            _wcsicmp(loaded.data(),identity.path.c_str())!=0) throw std::runtime_error("OFA loaded path mismatch");
        using Create=NV_OF_STATUS(NVOFAPI*)(uint32_t,NV_OF_D3D12_API_FUNCTION_LIST*);
        using Max=NV_OF_STATUS(NVOFAPI*)(uint32_t*);
        const auto create=reinterpret_cast<Create>(GetProcAddress(module,"NvOFAPICreateInstanceD3D12"));
        const auto maximum=reinterpret_cast<Max>(GetProcAddress(module,"NvOFGetMaxSupportedApiVersion"));
        if(!create || !maximum) throw std::runtime_error("OFA required export missing");
        of_checked(maximum(&identity.maxApi),"OFA maximum API query");
        if(identity.maxApi<NV_OF_API_VERSION) throw std::runtime_error("OFA API5 unavailable");
        of_checked(create(NV_OF_API_VERSION,&api),"OFA API5 table");
        if(!api.nvCreateOpticalFlowD3D12 || !api.nvOFInit || !api.nvOFGetCaps ||
           !api.nvOFGetSurfaceFormatCountD3D12 || !api.nvOFGetSurfaceFormatD3D12 ||
           !api.nvOFRegisterResourceD3D12 || !api.nvOFUnregisterResourceD3D12 || !api.nvOFExecuteD3D12 || !api.nvOFDestroy)
            throw std::runtime_error("OFA incomplete API table");
    } catch(...) { CloseHandle(file); throw; }
    CloseHandle(file);
}
ComPtr<ID3D12Resource> texture(ID3D12Device* device,Extent e,DXGI_FORMAT format,bool uav) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=e.width; d.Height=e.height;
    d.DepthOrArraySize=1; d.MipLevels=1; d.Format=format; d.SampleDesc.Count=1;
    d.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"guide texture");
    return r;
}
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource=r;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore=from; b.Transition.StateAfter=to;
    list->ResourceBarrier(1,&b);
}
} // private helpers
struct D3D12Guides::Impl {
    DriverModule driver;
    FlowCapabilities caps;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> ofFence,workerFence,currentReady,previousReady;
    ComPtr<ID3D12CommandAllocator> allocator,predictorAllocator,inputAllocator;
    ComPtr<ID3D12GraphicsCommandList> list,predictorList,inputList;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> hardware,cost,motion,depth,current,previous,bootstrap,bootstrapCost,global,hints;
    ComPtr<ID3D12Resource> bootstrapCurrent,bootstrapPrevious;
    std::array<NvOFGPUBufferHandle,10> registered{};
    NvOFHandle session=nullptr,refinement=nullptr;
    const Extent size;
    uint32_t grid=0,hintGrid=0;
    DXGI_FORMAT costFormat=DXGI_FORMAT_UNKNOWN;
    uint64_t ofValue=0,workerValue=0;
    bool pending=false,uncertain=false,disableHints=true,closed=false;
    std::optional<JobKey> last;
    std::optional<Rect> lastActive;
    std::optional<FlowResult> output;
    explicit Impl(ID3D12Device* d,ID3D12CommandQueue* q,Extent s):device(d),queue(q),size(s) {}
    std::vector<DXGI_FORMAT> formats(NV_OF_BUFFER_USAGE usage) {
        uint32_t count=0;
        of_checked(driver.api.nvOFGetSurfaceFormatCountD3D12(session,usage,NV_OF_MODE_OPTICALFLOW,&count),"OFA format count");
        if(count>256) throw std::runtime_error("OFA malformed format capabilities");
        if(!count) return {};
        std::vector<DXGI_FORMAT> values(count);
        of_checked(driver.api.nvOFGetSurfaceFormatD3D12(session,usage,NV_OF_MODE_OPTICALFLOW,values.data()),"OFA formats");
        return values;
    }
    std::vector<uint32_t> capability(NV_OF_CAPS name) {
        uint32_t count=0;
        of_checked(driver.api.nvOFGetCaps(session,name,nullptr,&count),"OFA predictor capability count");
        if(count>256) throw std::runtime_error("OFA malformed predictor capabilities");
        if(!count) return {};
        std::vector<uint32_t> values(count); const auto allocated=count;
        of_checked(driver.api.nvOFGetCaps(session,name,values.data(),&count),"OFA predictor capabilities");
        if(count>allocated) throw std::runtime_error("OFA predictor count changed");
        values.resize(count); return values;
    }
    void initialize(std::span<const std::byte> shader) {
        if(!device || !queue || !size.width || !size.height || shader.empty() ||
           queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) throw std::runtime_error("owned guide device/queue admission");
        ComPtr<ID3D12Device> queueDevice; checked(queue->GetDevice(IID_PPV_ARGS(&queueDevice)),"guide queue device");
        ComPtr<IUnknown> a,b; checked(device.As(&a),"guide device identity"); checked(queueDevice.As(&b),"guide queue identity");
        if(a.Get()!=b.Get()) throw std::runtime_error("guide queue is not on owned device");
        driver.load(); of_checked(driver.api.nvCreateOpticalFlowD3D12(device.Get(),&session),"OFA create");
        uint32_t count=0;
        of_checked(driver.api.nvOFGetCaps(session,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,nullptr,&count),"OFA grid count");
        if(!count || count>256) throw std::runtime_error("OFA empty/malformed grid capabilities");
        std::vector<uint32_t> grids(count); const uint32_t allocated=count;
        of_checked(driver.api.nvOFGetCaps(session,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,grids.data(),&count),"OFA grids");
        if(!count || count>allocated) throw std::runtime_error("OFA grid count changed");
        grids.resize(count); const auto smallest=smallest_flow_grid(grids);
        caps.grids=grids;
        if(!smallest) throw std::runtime_error("OFA unsupported output grid"); grid=*smallest;
        const auto inputFormats=formats(NV_OF_BUFFER_USAGE_INPUT),outputFormats=formats(NV_OF_BUFFER_USAGE_OUTPUT);
        caps.inputFormats=inputFormats; caps.outputFormats=outputFormats;
        if(std::find(inputFormats.begin(),inputFormats.end(),DXGI_FORMAT_B8G8R8A8_UNORM)==inputFormats.end() ||
           std::find(outputFormats.begin(),outputFormats.end(),DXGI_FORMAT_R16G16_SINT)==outputFormats.end())
            throw std::runtime_error("OFA BGRA8/SHORT2 format unsupported");
        const auto costs=formats(NV_OF_BUFFER_USAGE_COST);
        caps.costFormats=costs;
        for(auto wanted:{DXGI_FORMAT_R8_UINT,DXGI_FORMAT_R32_UINT})
            if(std::find(costs.begin(),costs.end(),wanted)!=costs.end()) { costFormat=wanted; break; }
        if(!flow_cost_format_valid(costFormat)) throw std::runtime_error("OFA cost capability unsupported");
        caps.hintSupport=capability(NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE);
        caps.hintGrids=capability(NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES);
        caps.hintFormats=formats(NV_OF_BUFFER_USAGE_HINT);
        caps.globalFormats=formats(NV_OF_BUFFER_USAGE_GLOBAL_FLOW);
        const auto admittedHint=smallest_hint_grid(caps.hintGrids,grid);
        if(caps.hintSupport!=std::vector<uint32_t>{1} || !admittedHint ||
           std::find(caps.hintFormats.begin(),caps.hintFormats.end(),DXGI_FORMAT_R16G16_SINT)==caps.hintFormats.end() ||
           std::find(caps.globalFormats.begin(),caps.globalFormats.end(),DXGI_FORMAT_R16G16_SINT)==caps.globalFormats.end())
            throw std::runtime_error("OFA native predictor capability unsupported");
        hintGrid=*admittedHint;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT hintSupport{DXGI_FORMAT_R16G16_SINT};
        checked(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&hintSupport,sizeof(hintSupport)),"predictor typed UAV query");
        if(!(hintSupport.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) throw std::runtime_error("predictor typed UAV store unsupported");
        const auto init=flow_bootstrap_params(size,grid); of_checked(driver.api.nvOFInit(session,&init),"OFA bootstrap initialize");
        of_checked(driver.api.nvCreateOpticalFlowD3D12(device.Get(),&refinement),"OFA refinement create");
        const auto refineInit=flow_refinement_params(size,grid,hintGrid);
        of_checked(driver.api.nvOFInit(refinement,&refineInit),"OFA refinement initialize");
        const Extent cells{(size.width+grid-1)/grid,(size.height+grid-1)/grid};
        hardware=texture(device.Get(),cells,DXGI_FORMAT_R16G16_SINT,false);
        cost=texture(device.Get(),cells,costFormat,false);
        bootstrap=texture(device.Get(),cells,DXGI_FORMAT_R16G16_SINT,false);
        bootstrapCost=texture(device.Get(),cells,costFormat,false);
        global=texture(device.Get(),{1,1},DXGI_FORMAT_R16G16_SINT,false);
        hints=texture(device.Get(),{(size.width+hintGrid-1)/hintGrid,(size.height+hintGrid-1)/hintGrid},DXGI_FORMAT_R16G16_SINT,true);
        bootstrapCurrent=texture(device.Get(),size,DXGI_FORMAT_B8G8R8A8_UNORM,false);
        bootstrapPrevious=texture(device.Get(),size,DXGI_FORMAT_B8G8R8A8_UNORM,false);
        motion=texture(device.Get(),size,DXGI_FORMAT_R16G16_FLOAT,true);
        depth=texture(device.Get(),size,DXGI_FORMAT_R32_FLOAT,true);
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ofFence)),"OFA private fence");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&workerFence)),"guide worker fence");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"guide allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"guide list");
        checked(list->Close(),"guide initial list close");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&predictorAllocator)),"predictor allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,predictorAllocator.Get(),nullptr,IID_PPV_ARGS(&predictorList)),"predictor list");
        checked(predictorList->Close(),"predictor initial close");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&inputAllocator)),"bootstrap input allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,inputAllocator.Get(),nullptr,IID_PPV_ARGS(&inputList)),"bootstrap input list");
        checked(inputList->Close(),"bootstrap input initial close");
        create_pipeline(shader);
    }
    void create_pipeline(std::span<const std::byte>);
    bool image_valid(const FlowImage&) const;
    bool register_image(NvOFHandle,ID3D12Resource*,NV_OF_FENCE_POINT,size_t);
    bool unregister_all();
    bool close();
    FlowProblem submit(const FlowInput&);
};
void D3D12Guides::Impl::create_pipeline(std::span<const std::byte> shader) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors=1;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors=3;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues=8;
    for(UINT i=0;i<2;++i) {
        parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i+1].DescriptorTable.NumDescriptorRanges=1;
        parameters[i+1].DescriptorTable.pDescriptorRanges=&ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters=3; signature.pParameters=parameters;
    ComPtr<ID3DBlob> blob,error;
    checked(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"guide root serialize");
    checked(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"guide root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=root.Get(); pso.CS={shader.data(),shader.size()};
    checked(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline)),"guide PSO");
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=5; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    checked(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"guide descriptor heap");
    const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R16G16_SINT;
    srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels=1; device->CreateShaderResourceView(hardware.Get(),&srv,cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    cpu.ptr+=stride; uav.Format=DXGI_FORMAT_R16G16_FLOAT; device->CreateUnorderedAccessView(motion.Get(),nullptr,&uav,cpu);
    cpu.ptr+=stride; uav.Format=DXGI_FORMAT_R32_FLOAT; device->CreateUnorderedAccessView(depth.Get(),nullptr,&uav,cpu);
    cpu.ptr+=stride; uav.Format=DXGI_FORMAT_R16G16_SINT; device->CreateUnorderedAccessView(hints.Get(),nullptr,&uav,cpu);
    cpu.ptr+=stride; device->CreateShaderResourceView(global.Get(),&srv,cpu);
}
bool D3D12Guides::Impl::image_valid(const FlowImage& image) const {
    if(!image.analysis || !image.ready.fence || !image.ready.value || image.ready.value==UINT64_MAX) return false;
    const auto desc=image.analysis->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM ||
       desc.Width!=size.width || desc.Height!=size.height || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 ||
       desc.SampleDesc.Count!=1 || image.ready.fence->GetCompletedValue()==UINT64_MAX) return false;
    ComPtr<ID3D12Device> resourceDevice,fenceDevice;
    if(FAILED(image.analysis->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
       FAILED(image.ready.fence->GetDevice(IID_PPV_ARGS(&fenceDevice)))) return false;
    ComPtr<IUnknown> owned,resource,fence;
    return SUCCEEDED(device.As(&owned)) && SUCCEEDED(resourceDevice.As(&resource)) && SUCCEEDED(fenceDevice.As(&fence)) &&
           owned.Get()==resource.Get() && owned.Get()==fence.Get();
}
bool D3D12Guides::Impl::register_image(NvOFHandle owner,ID3D12Resource* resource,NV_OF_FENCE_POINT ready,size_t index) {
    if(ofValue==UINT64_MAX-1) { uncertain=true; return false; }
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p{};
    p.resource=resource; p.inputFencePoint=ready; p.hOFGpuBuffer=&registered[index];
    p.outputFencePoint={ofFence.Get(),++ofValue};
    if(driver.api.nvOFRegisterResourceD3D12(owner,&p)!=NV_OF_SUCCESS) { uncertain=true; return false; }
    return true;
}
bool D3D12Guides::Impl::unregister_all() {
    for(auto& handle:registered) if(handle) {
        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{}; p.hOFGpuBuffer=handle;

        const auto status=driver.api.nvOFUnregisterResourceD3D12(&p);

        if(status!=NV_OF_SUCCESS) { uncertain=true; return false; }
        handle=nullptr;
    }
    return true;
}
bool D3D12Guides::Impl::close() {
    if(closed) return true;
    if(pending || uncertain) return false;
    if(!unregister_all()) return false;
    // ALL registered allocations release before EITHER session (Programming Guide6.2 / observed A/B).
    hardware.Reset(); cost.Reset(); bootstrap.Reset(); bootstrapCost.Reset(); global.Reset(); hints.Reset();
    bootstrapCurrent.Reset(); bootstrapPrevious.Reset();
    for(auto* owner:{&refinement,&session}) if(*owner) {
        if(driver.api.nvOFDestroy(*owner)!=NV_OF_SUCCESS) { uncertain=true; return false; }
        *owner=nullptr;
    }
    closed=true; return true;
}
FlowProblem D3D12Guides::Impl::submit(const FlowInput& in) {
    if(closed || pending || uncertain) return FlowProblem::Busy;
    const auto problem=flow_chronology_problem(in.current.key,in.previous?&in.previous->key:nullptr,
        last?&*last:nullptr,in.active,lastActive?&*lastActive:nullptr,in.reset);
    if(problem!=FlowProblem::None) return problem;
    if(in.working!=size || !flow_geometry_valid(size,in.active) || !image_valid(in.current) ||
       (in.previous && (!image_valid(*in.previous) || in.current.analysis==in.previous->analysis)))
        return FlowProblem::InvalidResource;
    if(workerValue>UINT64_MAX-4 || ofValue>UINT64_MAX-14) return FlowProblem::ApiFailure;
    current=in.current.analysis; previous=in.previous?in.previous->analysis:nullptr;
    currentReady=in.current.ready.fence; previousReady=in.previous?in.previous->ready.fence:nullptr;
    pending=true; // Ownership registered BEFORE any asynchronous GPU/API operation.
    try {
        checked(queue->Wait(in.current.ready.fence,in.current.ready.value),"guide current producer dependency");
        if(in.previous) checked(queue->Wait(in.previous->ready.fence,in.previous->ready.value),"guide previous producer dependency");
        if(!in.reset) {
            checked(inputAllocator->Reset(),"bootstrap input allocator reset"); checked(inputList->Reset(inputAllocator.Get(),nullptr),"bootstrap input list reset");
            const std::array<ID3D12Resource*,2> sources{current.Get(),previous.Get()},copies{bootstrapCurrent.Get(),bootstrapPrevious.Get()};
            for(size_t i=0;i<2;++i) {
                barrier(inputList.Get(),sources[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
                barrier(inputList.Get(),copies[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
                inputList->CopyResource(copies[i],sources[i]);
                barrier(inputList.Get(),sources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
                barrier(inputList.Get(),copies[i],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
            }
            checked(inputList->Close(),"bootstrap input list close");
            ID3D12CommandList* inputLists[]{inputList.Get()}; queue->ExecuteCommandLists(1,inputLists);
        }
        checked(queue->Signal(workerFence.Get(),++workerValue),"guide input admission signal");
        NV_OF_FENCE_POINT admitted{workerFence.Get(),workerValue};
        NV_OF_FENCE_POINT bootstrapped{},predicted{},finished{};
        if(!in.reset) {
            if(!register_image(session,bootstrapCurrent.Get(),admitted,0) ||
               !register_image(session,bootstrapPrevious.Get(),{ofFence.Get(),ofValue},1) ||
               !register_image(session,bootstrap.Get(),{ofFence.Get(),ofValue},2) ||
               !register_image(session,bootstrapCost.Get(),{ofFence.Get(),ofValue},3) ||
               !register_image(session,global.Get(),{ofFence.Get(),ofValue},4)) return FlowProblem::ApiFailure;
            std::array<NV_OF_FENCE_POINT,1> deps{{{ofFence.Get(),ofValue}}};
            const auto input=flow_execute_input(registered[0],registered[1],disableHints,deps);
            bootstrapped={ofFence.Get(),++ofValue};
            NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 result{};
            result.outputBuffer=registered[2]; result.outputCostBuffer=registered[3];
            result.globalFlowBuffer=registered[4]; result.fencePoint=&bootstrapped;
            if(driver.api.nvOFExecuteD3D12(session,&input,&result)!=NV_OF_SUCCESS) {
                uncertain=true; return FlowProblem::ApiFailure;
            }
            checked(queue->Wait(bootstrapped.fence,bootstrapped.value),"predictor bootstrap dependency");
            checked(predictorAllocator->Reset(),"predictor allocator reset");
            checked(predictorList->Reset(predictorAllocator.Get(),pipeline.Get()),"predictor list reset");
            barrier(predictorList.Get(),global.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(predictorList.Get(),hints.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* predictorHeaps[]{heap.Get()}; predictorList->SetDescriptorHeaps(1,predictorHeaps);
            predictorList->SetComputeRootSignature(root.Get());
            const uint32_t hintWidth=(size.width+hintGrid-1)/hintGrid,hintHeight=(size.height+hintGrid-1)/hintGrid;
            const std::array<uint32_t,8> predictorParams{hintWidth,hintHeight,hintGrid,2,0,0,0,0};
            predictorList->SetComputeRoot32BitConstants(0,8,predictorParams.data(),0);
            const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto predictorGpu=heap->GetGPUDescriptorHandleForHeapStart(); predictorGpu.ptr+=4ull*stride;
            predictorList->SetComputeRootDescriptorTable(1,predictorGpu);
            predictorGpu=heap->GetGPUDescriptorHandleForHeapStart(); predictorGpu.ptr+=stride;
            predictorList->SetComputeRootDescriptorTable(2,predictorGpu);
            predictorList->Dispatch((hintWidth+7)/8,(hintHeight+7)/8,1);
            barrier(predictorList.Get(),global.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
            barrier(predictorList.Get(),hints.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
            checked(predictorList->Close(),"predictor list close");
            ID3D12CommandList* predictorLists[]{predictorList.Get()}; queue->ExecuteCommandLists(1,predictorLists);
            checked(queue->Signal(workerFence.Get(),++workerValue),"predictor completion signal");
            predicted={workerFence.Get(),workerValue};
            if(!register_image(refinement,current.Get(),predicted,5) ||
               !register_image(refinement,previous.Get(),{ofFence.Get(),ofValue},6) ||
               !register_image(refinement,hardware.Get(),{ofFence.Get(),ofValue},7) ||
               !register_image(refinement,cost.Get(),{ofFence.Get(),ofValue},8) ||
               !register_image(refinement,hints.Get(),{ofFence.Get(),ofValue},9)) return FlowProblem::ApiFailure;
            deps[0]={ofFence.Get(),ofValue};
            const auto refineInput=flow_execute_input(registered[5],registered[6],true,deps,registered[9]);
            finished={ofFence.Get(),++ofValue};
            result={}; result.outputBuffer=registered[7]; result.outputCostBuffer=registered[8]; result.fencePoint=&finished;
            if(driver.api.nvOFExecuteD3D12(refinement,&refineInput,&result)!=NV_OF_SUCCESS) {
                uncertain=true; return FlowProblem::ApiFailure;
            }
            checked(queue->Wait(finished.fence,finished.value),"guide OFA conversion dependency");
        }
        checked(allocator->Reset(),"guide allocator reset"); checked(list->Reset(allocator.Get(),pipeline.Get()),"guide list reset");
        barrier(list.Get(),hardware.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list.Get(),motion.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(list.Get(),depth.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]{heap.Get()}; list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootSignature(root.Get());
        const std::array<uint32_t,8> params{size.width,size.height,grid,in.reset?1u:0u,
            in.active.x,in.active.y,in.active.width,in.active.height};
        list->SetComputeRoot32BitConstants(0,8,params.data(),0);
        auto gpu=heap->GetGPUDescriptorHandleForHeapStart(); list->SetComputeRootDescriptorTable(1,gpu);
        gpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetComputeRootDescriptorTable(2,gpu);
        list->Dispatch((size.width+7)/8,(size.height+7)/8,1);
        barrier(list.Get(),hardware.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        barrier(list.Get(),motion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        barrier(list.Get(),depth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        checked(list->Close(),"guide submit list close");
        ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
        checked(queue->Signal(workerFence.Get(),++workerValue),"guide conversion completion signal");
        std::array<uintptr_t,10> registeredIds{};
        for(size_t i=0;i<registered.size();++i) registeredIds[i]=reinterpret_cast<uintptr_t>(registered[i]);
        output.emplace(FlowResult{in.current.key,motion.Get(),depth.Get(),in.reset?nullptr:cost.Get(),in.reset?nullptr:hardware.Get(),
                                 {workerFence.Get(),workerValue},in.reset,
                                 in.reset?nullptr:bootstrap.Get(),in.reset?nullptr:global.Get(),in.reset?nullptr:hints.Get(),
                                 admitted,bootstrapped,predicted,finished,in.current.ready,in.previous?in.previous->ready:NV_OF_FENCE_POINT{},
                                 in.reset?nullptr:bootstrapCurrent.Get(),in.reset?nullptr:bootstrapPrevious.Get(),registeredIds});
        last.emplace(in.current.key); lastActive.emplace(in.active); disableHints=in.reset;
        return FlowProblem::None;
    } catch(const std::exception& error) {
        OutputDebugStringA(error.what()); uncertain=true; return FlowProblem::ApiFailure;
    }
}
D3D12Guides::D3D12Guides(ID3D12Device* device,ID3D12CommandQueue* queue,Extent size,std::span<const std::byte> shader)
    :impl_(std::make_unique<Impl>(device,queue,size)) {
    try { impl_->initialize(shader); }
    catch(...) { if(!impl_->close()) { OutputDebugStringA("OFA initialization ownership retained"); (void)impl_.release(); } throw; }
}
D3D12Guides::~D3D12Guides() {
    if(impl_ && !impl_->close()) {
        OutputDebugStringA("OFA unresolved ownership retained; no module unload/resource release");
        (void)impl_.release(); // Explicit process-lifetime quarantine, not successful cleanup.
    }
}
FlowProblem D3D12Guides::submit(const FlowInput& input) { return impl_->submit(input); }
const FlowResult* D3D12Guides::result() const { return impl_->uncertain?nullptr:(impl_->output?&*impl_->output:nullptr); }
bool D3D12Guides::collect(const JobKey& key,NV_OF_FENCE_POINT consumer) {
    if(!impl_->pending || impl_->uncertain || !impl_->output || key!=impl_->output->key ||
       !completed(impl_->output->complete) || !completed(consumer) ||
       (impl_->ofValue && !completed({impl_->ofFence.Get(),impl_->ofValue}))) return false;
    if(!impl_->unregister_all()) return false;
    impl_->current.Reset(); impl_->previous.Reset(); impl_->currentReady.Reset(); impl_->previousReady.Reset();
    impl_->output.reset(); impl_->pending=false; return true;
}
bool D3D12Guides::close() { return impl_->close(); }
const FlowModuleIdentity& D3D12Guides::module_identity() const { return impl_->driver.identity; }
const FlowCapabilities& D3D12Guides::capabilities() const { return impl_->caps; }
uint32_t D3D12Guides::grid_size() const { return impl_->grid; }
uint32_t D3D12Guides::hint_grid_size() const { return impl_->hintGrid; }
DXGI_FORMAT D3D12Guides::cost_format() const { return impl_->costFormat; }
} // nr
