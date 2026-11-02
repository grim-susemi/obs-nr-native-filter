// SPDX-License-Identifier: GPL-3.0-only
#include "nr_runtime.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace {
unsigned checks=0;
void expect(bool condition,const char* text){++checks;if(!condition)throw std::runtime_error(text);}
void registration() {
    using nr::runtime::DriverRegistration;
    const DriverRegistration valid{L"PCI\\VEN_10DE&DEV_2684\\actual",L"{display}\\0000",
        L"oem1.inf",L"nv_registered_package",L"32.0.16.1714",L"Section061"};
    std::array<DriverRegistration,1> one{valid};
    expect(nr::runtime::driver_registration(one).activePackage==valid.activePackage,"registered package selection");
    const auto rejected=[&](std::span<const DriverRegistration> rows) {
        bool failed=false;try{(void)nr::runtime::driver_registration(rows);}catch(const std::runtime_error&){failed=true;}
        expect(failed,"missing/ambiguous/unbound registration accepted");
    };
    rejected({});
    const std::array<DriverRegistration,2> duplicate{valid,valid};rejected(duplicate);
    for(unsigned field=0;field<6;++field) {
        one[0]=valid;
        if(field==0)one[0].deviceInstance=L"PCI\\VEN_8086&DEV_1234\\other";
        if(field==1)one[0].driverKey.clear();
        if(field==2)one[0].publishedInf=L"..\\oem1.inf";
        if(field==3)one[0].activePackage=L"../unregistered";
        if(field==4)one[0].driverVersion=L"32..16.1714";
        if(field==5)one[0].infSection.clear();
        rejected(one);
    }
    for(const auto* version:{L"",L"...",L"32.0.16",L"32.0.16.1714.1",L"32.0.16.65536",L"32.0.16.bad"}) {
        one[0]=valid;one[0].driverVersion=version;rejected(one);
    }
    const std::filesystem::path infDirectory=L"C:/Windows/INF";
    expect(nr::runtime::published_inf_path(infDirectory,L"oem67.inf")==infDirectory/L"oem67.inf",
        "SetupAPI published basename resolved against process cwd");
    for(const auto* name:{L"",L"..",L"../oem67.inf",L"..\\oem67.inf",L"C:\\other\\oem67.inf",L"oem67.dll",L"oem67.inf\n"}) {
        bool failed=false;
        try{(void)nr::runtime::published_inf_path(infDirectory,name);}catch(const std::runtime_error&){failed=true;}
        expect(failed,"unbound published INF name accepted");
    }
    bool relative=false;
    try{(void)nr::runtime::published_inf_path(L"relative-INF",L"oem67.inf");}catch(const std::runtime_error&){relative=true;}
    expect(relative,"relative Windows INF directory accepted");
}
void pe() {
    std::vector<unsigned char> file(0x1400);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data()+0x80);nt->Signature=IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->FileHeader.NumberOfSections=1;nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfHeaders=0x400;nt->OptionalHeader.NumberOfRvaAndSizes=IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]={0x1000,0x80};
    auto* section=reinterpret_cast<IMAGE_SECTION_HEADER*>(file.data()+0x80+sizeof(*nt));
    section->VirtualAddress=0x1000;section->PointerToRawData=0x400;section->SizeOfRawData=0x1000;
    auto* exports=reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(file.data()+0x400);
    exports->NumberOfFunctions=exports->NumberOfNames=7;
    exports->AddressOfFunctions=0x1100;exports->AddressOfNames=0x1140;exports->AddressOfNameOrdinals=0x1180;
    const char* names[]{"NVSDK_NGX_D3D12_Init_Ext","NVSDK_NGX_D3D12_CreateFeature",
        "NVSDK_NGX_D3D12_EvaluateFeature","NVSDK_NGX_D3D12_ReleaseFeature","NVSDK_NGX_D3D12_Shutdown1",
        "NVSDK_NGX_D3D12_GetCapabilityParameters","NVSDK_NGX_D3D12_DestroyParameters"};
    for(unsigned i=0;i<7;++i) {
        const DWORD function=0x1800+i,name=0x1200+i*0x60;const WORD ordinal=static_cast<WORD>(i);
        memcpy(file.data()+0x500+i*4,&function,4);memcpy(file.data()+0x540+i*4,&name,4);
        memcpy(file.data()+0x580+i*2,&ordinal,2);memcpy(file.data()+0x600+i*0x60,names[i],strlen(names[i])+1);
    }
    expect(nr::runtime::inspect_driver_pe(file).accepted,"complete direct CORE exports");
    expect(nr::runtime::inspect_pe(file).accepted,"shared NR PE contract changed");
    for(unsigned i=0;i<7;++i) {
        const auto at=0x600+i*0x60;const auto first=file[at];file[at]='X';
        expect(!nr::runtime::inspect_driver_pe(file).accepted,"missing required CORE export accepted");file[at]=first;
        const DWORD forwarded=0x1010,function=0x1800+i;
        memcpy(file.data()+0x500+i*4,&forwarded,4);
        expect(!nr::runtime::inspect_driver_pe(file).accepted,"forwarded required CORE export accepted");
        memcpy(file.data()+0x500+i*4,&function,4);
    }
    nt->FileHeader.Machine=IMAGE_FILE_MACHINE_I386;
    expect(!nr::runtime::inspect_driver_pe(file).accepted,"non-AMD64 CORE accepted");
    nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;dos->e_lfanew=-1;
    expect(!nr::runtime::inspect_driver_pe(file).accepted,"bad CORE PE accepted");
    expect(!nr::runtime::inspect_driver_pe(std::span<const unsigned char>{file}.first(10)).accepted,"partial CORE PE accepted");
}
}
int main() {
    try {
        registration();pe();
        std::printf("PASS runtime_driver_binding checks=%u DLL_loads=0 GPU_calls=0\n",checks);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL runtime_driver_binding %s checks=%u\n",error.what(),checks);return 1;}
}
