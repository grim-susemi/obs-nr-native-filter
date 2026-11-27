// SPDX-License-Identifier: GPL-3.0-only
// Real hardware probe: the admitted 310.9.16.0 NR runtime must select Model B
// (CC_SMALL_FP8) for exactly one DLSSNR.Hint.Render.Preset integer. This binary
// records that integer as evidence; it never runs the OBS filter or a game.
#include "nr_runtime.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
using namespace nr;
static unsigned checks=0;
static void check(bool value,const char* what)
{
    ++checks;
    if(!value)throw std::runtime_error(std::string("probe assertion: ")+what);
}
static void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,after};
    list->ResourceBarrier(1,&b);
}
struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;HANDLE event=nullptr;uint64_t serial=0;
    ComPtr<ID3D12Resource> color,depth,motion,output;
    Gpu()
    {
        ComPtr<IDXGIFactory4> factory;
        check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"DXGI factory");
        ComPtr<IDXGIAdapter1> chosen;
        for(UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
            if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            if(!chosen)chosen=adapter;
            if(desc.VendorId==0x10DE){chosen=adapter;break;}
        }
        check(chosen && SUCCEEDED(D3D12CreateDevice(chosen.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"D3D12 device");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"command queue");
        check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"allocator");
        check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"command list");
        check(SUCCEEDED(list->Close()),"initial list close");
        check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"fence event");
        color=texture(DXGI_FORMAT_R16G16B16A16_FLOAT);depth=texture(DXGI_FORMAT_R32_FLOAT);
        motion=texture(DXGI_FORMAT_R16G16_FLOAT);output=texture(DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    ~Gpu(){if(event)CloseHandle(event);}
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format)
    {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=1920;d.Height=1080;
        d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,
            nullptr,IID_PPV_ARGS(&r))),"probe texture");
        return r;
    }
    void reset(){check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"list reset");}
    void execute()
    {
        check(SUCCEEDED(list->Close()),"list close");ID3D12CommandList* commands=list.Get();
        queue->ExecuteCommandLists(1,&commands);
        check(SUCCEEDED(queue->Signal(fence.Get(),++serial)),"fence signal");
        if(fence->GetCompletedValue()<serial) {
            check(SUCCEEDED(fence->SetEventOnCompletion(serial,event)),"fence event completion");
            check(WaitForSingleObject(event,15000)==WAIT_OBJECT_0,"bounded GPU wait");
        }
    }
};
struct Core {
    HMODULE module=nullptr;runtime::Dispatch dispatch;std::wstring features;
    using Init=NVSDK_NGX_Result(*)(unsigned long long,const wchar_t*,ID3D12Device*,NVSDK_NGX_Version,
        const NVSDK_NGX_FeatureCommonInfo*);
    using Shutdown=NVSDK_NGX_Result(*)(ID3D12Device*,unsigned int&);
    Init init=nullptr;Shutdown shutdown=nullptr;ID3D12Device* device=nullptr;
    template<class T> static T symbol(HMODULE m,const char* n)
    {auto* v=GetProcAddress(m,n);check(v!=nullptr,n);return reinterpret_cast<T>(v);}
    Core(const fs::path& path,const fs::path& featureDir,ID3D12Device* dev,const fs::path& data):device(dev)
    {
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module!=nullptr,"NGX core load");
        dispatch.allocate=symbol<decltype(dispatch.allocate)>(module,"NVSDK_NGX_D3D12_GetCapabilityParameters");
        dispatch.destroy=symbol<decltype(dispatch.destroy)>(module,"NVSDK_NGX_D3D12_DestroyParameters");
        dispatch.create=symbol<decltype(dispatch.create)>(module,"NVSDK_NGX_D3D12_CreateFeature");
        dispatch.evaluate=symbol<decltype(dispatch.evaluate)>(module,"NVSDK_NGX_D3D12_EvaluateFeature");
        dispatch.release=symbol<decltype(dispatch.release)>(module,"NVSDK_NGX_D3D12_ReleaseFeature");
        init=symbol<Init>(module,"NVSDK_NGX_D3D12_Init_Ext");
        shutdown=symbol<Shutdown>(module,"NVSDK_NGX_D3D12_Shutdown1");
        features=featureDir.wstring();
        const wchar_t* paths[]{features.c_str()};
        NVSDK_NGX_FeatureCommonInfo info{};info.PathListInfo.Path=paths;info.PathListInfo.Length=1;
        check(init(0x24480451ull,data.c_str(),device,NVSDK_NGX_Version_API,&info)==NVSDK_NGX_Result_Success,"NGX core Init_Ext");
    }
    void close()
    {
        if(module) {
            if(shutdown) {unsigned remaining=0;(void)shutdown(device,remaining);}
            FreeLibrary(module);module=nullptr;
        }
    }
    ~Core(){close();}
};
struct Leased {
    std::shared_ptr<int> lease=std::make_shared<int>(18);
    std::shared_ptr<runtime::Module> module;
    std::vector<std::unique_ptr<runtime::Pass>> passes;
    ~Leased(){passes.clear();if(module)(void)module->close();}
};
static Settings settings_for(const fs::path& runtime,unsigned preset)
{
    return {{1},true,runtime.generic_string(),{1.0f},{preset,0,1.0f,1.0f,0.0f,-1.0f,true}};
}
static bool evaluate_preset(Leased& owner,const fs::path& runtime,unsigned preset,Gpu& gpu)
{
    const auto settings=settings_for(runtime,preset);
    auto pass=std::make_unique<runtime::Pass>(owner.module);
    auto complete=[] {return runtime::Completion::Complete;};
    const auto creationEpoch=10u+preset*2u;
    gpu.reset();
    const auto prepared=pass->prepare(settings,settings.tuning,{1920,1080},1,
        {gpu.list.Get(),creationEpoch,owner.lease,complete});
    if(prepared!=NVSDK_NGX_Result_Success)return false;
    gpu.execute();
    check(pass->ready(creationEpoch+1),"creation completion");
    const JobKey key{{1,1,1},1,1,1};
    const IngressSample input{key.frame,1,0,1,1,1,SourceValidity::Valid,
        {{1920,1080},{0,0,1920,1080},PixelFormat::Rgba8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},settings};
    const JobContract job{key,input,2};
    const runtime::Inputs params{gpu.color.Get(),gpu.depth.Get(),gpu.motion.Get(),gpu.output.Get(),
        {0,0,1920,1080},{0,0,1920,1080},false,true,1,1,true};
    gpu.reset();
    barrier(gpu.list.Get(),gpu.color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(gpu.list.Get(),gpu.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(gpu.list.Get(),gpu.motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(gpu.list.Get(),gpu.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto evaluated=pass->run(job,params,{gpu.list.Get(),creationEpoch+1,owner.lease,complete});
    if(evaluated!=NVSDK_NGX_Result_Success)return false;
    gpu.execute();
    const auto completed=pass->completed(key);
    if(!completed || !completed->evaluated)return false;
    check(pass->release(),"pass release");
    owner.passes.push_back(std::move(pass));
    return true;
}
static std::string text(const fs::path& path)
{
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),{}};
}
static std::string nr_log(const fs::path& directory)
{
    for(const auto& entry:fs::directory_iterator(directory))
        if(entry.path().filename().wstring().starts_with(L"nvngx_dlssnr_"))return text(entry.path());
    return {};
}
static void set_logging(const fs::path& directory)
{
    SetEnvironmentVariableW(L"__NGX_ENABLE_OVERRIDE_LOG_PATH",L"1");
    SetEnvironmentVariableW(L"__NGX_LOG_PATH_OVERRIDE",directory.c_str());
    SetEnvironmentVariableW(L"__NGX_LOG_LEVEL",L"2");
}
int main(int argc,char** argv)
{
    try {
        if(argc!=3 || std::string(argv[1])!="--require-runtime") {
            std::fprintf(stderr,"usage: nr_model_b_probe_tests --require-runtime <nvngx_dlssnr.dll>\n");
            return 2;
        }
        const auto runtime=fs::canonical(fs::path(argv[2]));
        const auto variant=runtime::allowed_runtimes().back();
        const std::string sha(variant.sha256),older(runtime::allowed_runtimes().front().sha256);
        check(variant.version=="310.9.16.0" && variant.supportsModelB,"310.9.16.0 allowlist entry");
        const auto started=std::chrono::steady_clock::now();
        const auto inspection=runtime::inspect({runtime,sha});
        const auto hashMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        check(inspection.accepted && inspection.sha256==variant.sha256,"real inspect accepts 310.9.16.0");
        check(inspection.exports.size()>=5,"PE64 export table");
        const auto cross=runtime::inspect({runtime,older});
        check(!cross.accepted && cross.reason=="hashMismatch" && cross.sha256==variant.sha256,"single-hash mismatch isolated");
        std::printf("inspect: sha256=%s bytes=276162560 hash_ms=%.1f exports=%zu\n",inspection.sha256.c_str(),hashMs,
            inspection.exports.size());
        for(const auto& name:inspection.exports)std::printf("export: %s\n",name.c_str());
        for(const auto* name:{"NVSDK_NGX_D3D12_Init_Ext","NVSDK_NGX_D3D12_CreateFeature","NVSDK_NGX_D3D12_EvaluateFeature","NVSDK_NGX_D3D12_ReleaseFeature","NVSDK_NGX_D3D12_Shutdown1"})
            check(std::find(inspection.exports.begin(),inspection.exports.end(),name)!=inspection.exports.end(),name);
        std::printf("required_exports: 5/5 PE64 direct exports present\n");
        check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM");
        const auto dataDir=fs::temp_directory_path()/("obs-nr-model-b-probe-"+std::to_string(GetCurrentProcessId()));
        fs::remove_all(dataDir);fs::create_directories(dataDir);set_logging(dataDir);
        Gpu gpu;
        const auto binding=runtime::resolve_driver();
        check(binding.signatureVerified,"signed active NVIDIA CORE");
        const auto emptyDir=fs::temp_directory_path()/("obs-nr-core-empty-"+std::to_string(GetCurrentProcessId()));
        fs::create_directories(emptyDir);
        Core core(binding.identity.canonicalPath,emptyDir,gpu.device.Get(),dataDir);
        std::printf("core: path=%s sha256=%s\n",binding.identity.canonicalPath.string().c_str(),binding.identity.sha256.c_str());
        Leased owner;
        owner.module=runtime::Module::open({runtime,sha},core.dispatch,owner.lease,gpu.device.Get(),
            dataDir,runtime::Route::DriverFallback);
        check(owner.module!=nullptr,"Module::open 310.9.16.0");
        std::printf("module: driver+openDirect fallback opened; 5-export PE64 and find_path_imports IAT check accepted\n");
        int modelA=-1,modelB=-1;std::string weight;
        for(unsigned preset=0;preset<=3;++preset) {
            const auto before=nr_log(dataDir).size();
            check(evaluate_preset(owner,runtime,preset,gpu),"prepare/evaluate");
            const auto slice=nr_log(dataDir).substr(before);
            const auto at=slice.rfind("DLSSNR: Created feature");
            check(at!=std::string::npos,"NGX feature log line");
            const auto end=slice.find('\n',at);
            const auto line=slice.substr(at,end==std::string::npos?std::string::npos:end-at);
            const bool fallbackLine=slice.find("falling back")!=std::string::npos;
            const bool modelBNetwork=slice.find("BuildActiveNetwork config=hnet-crazy-cuckoo-small-fp8")!=std::string::npos;
            const bool modelANetwork=slice.find("BuildActiveNetwork config=CC_Control_History")!=std::string::npos;
            check(modelBNetwork!=modelANetwork,"one NR network per preset");
            std::printf("preset=%u evaluate=ok fallback=%d network=%s log=%s\n",preset,fallbackLine?1:0,
                modelBNetwork?"small-fp8":"full-weights",line.c_str());
            if(!fallbackLine && modelBNetwork) {check(modelB<0,"unique directly available Model B preset");modelB=static_cast<int>(preset);weight="hnet-crazy-cuckoo-small-fp8";}
            if(!fallbackLine && modelANetwork) {check(modelA<0,"unique directly available Model A preset");modelA=static_cast<int>(preset);}
        }
        check(modelA==1 && modelB==2,"verified 310.9.16.0 mapping: preset 1 Model A, preset 2 Model B");
        std::printf("verified: model_api=0x%X model_a_preset=%d model_b_preset=%d model_b_weight=%s\n",0x15,modelA,modelB,weight.c_str());
        owner.passes.clear();
        check(owner.module->close(),"direct module close");
        owner.module.reset();
        core.close();
        Core driverCore(binding.identity.canonicalPath,runtime.parent_path(),gpu.device.Get(),dataDir);
        Leased driver;
        driver.module=runtime::Module::open({runtime,sha},driverCore.dispatch,driver.lease,gpu.device.Get(),
            dataDir,runtime::Route::Driver);
        check(driver.module!=nullptr,"Module::open driver route");
        const bool driverEvaluated=evaluate_preset(driver,runtime,static_cast<unsigned>(modelB),gpu);
        const auto coreLog=text(dataDir/"nvngx.log");
        const bool unsignedRefusal=coreLog.find("nvLoadSignedLibraryW() failed")!=std::string::npos;
        if(driverEvaluated) std::printf("driver_route: prepare/evaluate=ok preset=%d\n",modelB);
        else {
            check(unsignedRefusal,"driver-route refusal names the unsigned snippet");
            std::printf("driver_route: refused reason=unsigned_snippet; openDirect fallback is required\n");
        }
        driver.passes.clear();
        check(driver.module->close(),"driver module close");
        driver.module.reset();
        driverCore.close();
        std::printf("PASS nr_model_b_probe: %u checks, Model B preset %d\n",checks,modelB);
        fs::remove_all(emptyDir);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FAIL nr_model_b_probe: %s (%u checks)\n",e.what(),checks);
        return 1;
    }
}
