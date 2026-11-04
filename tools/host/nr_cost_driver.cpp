// SPDX-License-Identifier: GPL-3.0-only
#include "nr_cost_driver.h"
#include "nr_model.h"
#include <stdexcept>
#include <wrl/client.h>

namespace nr::qa {
using Microsoft::WRL::ComPtr;
struct CostDriver {
    ComPtr<ID3D12Device> device;
    HMODULE image=nullptr;
    HANDLE pinned=INVALID_HANDLE_VALUE;
    runtime::Dispatch api{};
    using Shutdown=NVSDK_NGX_Result (*)(ID3D12Device*,unsigned&);
    Shutdown shutdown=nullptr;
    bool initialized=false;
    bool usable=false;
    ~CostDriver() {
        // An unresolved vendor session keeps its exact image and device resident.
        if(initialized) { (void)device.Detach(); image=nullptr; pinned=INVALID_HANDLE_VALUE; }
        if(image) FreeLibrary(image);
        if(pinned!=INVALID_HANDLE_VALUE) CloseHandle(pinned);
    }
};
template<class T> T symbol(HMODULE module,const char* name) {
    auto address=GetProcAddress(module,name);
    if(!address) throw std::runtime_error(name);
    return reinterpret_cast<T>(address);
}
void open_cost_driver(std::shared_ptr<CostDriver>& owner,ID3D12Device* device,const std::filesystem::path& data) {
    owner=std::make_shared<CostDriver>();
    owner->device=device;
    const auto binding=runtime::resolve_driver();
    if(!binding.signatureVerified || !data.is_absolute() || !std::filesystem::is_directory(data))
        throw std::runtime_error("cost driver admission");
    owner->pinned=CreateFileW(binding.identity.canonicalPath.c_str(),GENERIC_READ,FILE_SHARE_READ,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(owner->pinned==INVALID_HANDLE_VALUE) throw std::runtime_error("cost driver pin");
    const auto fresh=runtime::resolve_driver();
    if(!fresh.signatureVerified || fresh.identity.sha256!=binding.identity.sha256 ||
        fresh.identity.canonicalPath!=binding.identity.canonicalPath) throw std::runtime_error("cost driver changed");
    owner->image=LoadLibraryExW(binding.identity.canonicalPath.c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!owner->image) throw std::runtime_error("cost driver load");
    using Init=NVSDK_NGX_Result (*)(unsigned long long,const wchar_t*,ID3D12Device*,
        NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
    const auto init=symbol<Init>(owner->image,"NVSDK_NGX_D3D12_Init_Ext");
    auto& d=owner->api;
    d.allocate=symbol<decltype(d.allocate)>(owner->image,"NVSDK_NGX_D3D12_GetCapabilityParameters");
    d.destroy=symbol<decltype(d.destroy)>(owner->image,"NVSDK_NGX_D3D12_DestroyParameters");
    d.create=symbol<decltype(d.create)>(owner->image,"NVSDK_NGX_D3D12_CreateFeature");
    d.evaluate=symbol<decltype(d.evaluate)>(owner->image,"NVSDK_NGX_D3D12_EvaluateFeature");
    d.release=symbol<decltype(d.release)>(owner->image,"NVSDK_NGX_D3D12_ReleaseFeature");
    owner->shutdown=symbol<CostDriver::Shutdown>(owner->image,"NVSDK_NGX_D3D12_Shutdown1");
    const auto path=data.wstring();
    const wchar_t* paths[]{path.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path=paths; info.PathListInfo.Length=1;
    // Preserve ownership if the foreign initialization fails after partially acquiring state.
    owner->initialized=true;
    const auto result=init(0x24480451ull,path.c_str(),device,NVSDK_NGX_Version_API,&info);
    if(result!=NVSDK_NGX_Result_Success) throw std::runtime_error("cost driver Init_Ext failed");
    owner->usable=true;
}
runtime::Dispatch cost_driver_dispatch(const std::shared_ptr<CostDriver>& owner) { return owner->api; }
bool close_cost_driver(const std::shared_ptr<CostDriver>& owner) {
    if(!owner->initialized) return true;
    if(!owner->usable) return false;
    if(model::entry_interposed(owner->image,reinterpret_cast<const void*>(owner->shutdown))) return false;
    unsigned remaining=0;
    if(owner->shutdown(owner->device.Get(),remaining)!=NVSDK_NGX_Result_Success || remaining) return false;
    owner->initialized=false;
    return true;
}
}
