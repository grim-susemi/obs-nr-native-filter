// SPDX-License-Identifier: GPL-3.0-only
// Read-only prerequisite inspector. Never loads CORE, creates a device or infers.
#include "nr_runtime.h"
#include <iomanip>
#include <iostream>
namespace {
std::string utf8(std::wstring_view value) {
    const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(!count)throw std::runtime_error("driver provenance UTF8");
    std::string text(static_cast<size_t>(count),'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),text.data(),count,nullptr,nullptr)!=count)
        throw std::runtime_error("driver provenance UTF8 extent");
    return text;
}
}
int main(int argc,char**) {
    try {
        if(argc!=1)throw std::runtime_error("driver inspector takes no overrides");
        const auto binding=nr::runtime::resolve_driver();const auto& registered=binding.registration;
        std::cout<<"{\"schema\":1,\"kind\":\"active-nvidia-driver-core\",\"loaded\":false,\"gpu_calls\":0,"
            <<"\"signature_verified\":"<<(binding.signatureVerified?"true":"false")<<",\"required_exports\":7,"
            <<"\"path\":"<<std::quoted(utf8(binding.identity.canonicalPath.wstring()))
            <<",\"sha256\":"<<std::quoted(binding.identity.sha256)
            <<",\"device_instance\":"<<std::quoted(utf8(registered.deviceInstance))
            <<",\"driver_key\":"<<std::quoted(utf8(registered.driverKey))
            <<",\"published_inf\":"<<std::quoted(utf8(registered.publishedInf))
            <<",\"active_package\":"<<std::quoted(utf8(registered.activePackage))
            <<",\"driver_store_inf\":"<<std::quoted(utf8(binding.driverStoreInf.wstring()))
            <<",\"driver_version\":"<<std::quoted(utf8(registered.driverVersion))
            <<",\"inf_section\":"<<std::quoted(utf8(registered.infSection))<<"}\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<"NR_DRIVER_BINDING_REFUSED: "<<error.what()<<'\n';return 1;
    }
}
