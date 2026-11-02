// SPDX-License-Identifier: GPL-3.0-only
// Port of R2: DlssNr_Proxy.cpp/.h, DlssNr_CompatibilityRuntime.cpp,
// DlssNr_RuntimeImports.h; OptiScaler NR fork (source-lock.json exact hashes).
// These source files contain no copyright/license banners. No holder is invented.
// GPL-3.0-only and attribution chain: provenance/source-notices/CREDITS.md,
// provenance/source-notices/GPL-3.0-NOTICE.txt. Owner model is not redistributed.
#include "nr_runtime.h"
#include <bcrypt.h>
#include <psapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wintrust.h>
#include <softpub.h>
#include <winver.h>
using Microsoft::WRL::ComPtr;
namespace nr::runtime {
namespace {
bool range(size_t offset,size_t length,size_t size) { return offset<=size && length<=size-offset; }
template<class T> bool read(std::span<const unsigned char> b,size_t at,T& out) {
    if(!range(at,sizeof(T),b.size())) return false;
    memcpy(&out,b.data()+at,sizeof(T)); return true;
}
bool headers(std::span<const unsigned char> b,IMAGE_NT_HEADERS64& nt,size_t& at) {
    IMAGE_DOS_HEADER dos{};
    if(!read(b,0,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0) return false;
    at=static_cast<size_t>(dos.e_lfanew);
    return read(b,at,nt) && nt.Signature==IMAGE_NT_SIGNATURE && nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64 &&
        nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC && nt.FileHeader.SizeOfOptionalHeader>=sizeof(IMAGE_OPTIONAL_HEADER64) &&
        nt.OptionalHeader.NumberOfRvaAndSizes>=IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
}
constexpr const char* required[]={"NVSDK_NGX_D3D12_Init_Ext","NVSDK_NGX_D3D12_CreateFeature",
    "NVSDK_NGX_D3D12_EvaluateFeature","NVSDK_NGX_D3D12_ReleaseFeature","NVSDK_NGX_D3D12_Shutdown1"};
struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~File() { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
std::vector<unsigned char> bytes(HANDLE handle) {
    LARGE_INTEGER n{};
    if(!GetFileSizeEx(handle,&n) || n.QuadPart<=0 || n.QuadPart>512*1024*1024) throw std::runtime_error("runtime size");
    std::vector<unsigned char> b(static_cast<size_t>(n.QuadPart)); DWORD got=0;
    if(!ReadFile(handle,b.data(),static_cast<DWORD>(b.size()),&got,nullptr) || got!=b.size()) throw std::runtime_error("runtime read");
    return b;
}
std::string digest(std::span<const unsigned char> b) {
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; unsigned char result[32]{};
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 provider");
    const auto created=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0);
    const bool ok=created>=0 && BCryptHashData(hash,const_cast<PUCHAR>(b.data()),static_cast<ULONG>(b.size()),0)>=0 &&
        BCryptFinishHash(hash,result,32,0)>=0;
    if(hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg,0);
    if(!ok) throw std::runtime_error("SHA256 failure");
    std::string out; constexpr char hex[]="0123456789abcdef";
    for(auto c:result) { out+=hex[c>>4]; out+=hex[c&15]; } return out;
}
}
static Inspection inspect_named_pe(std::span<const unsigned char> b,std::span<const char* const> requiredNames) {
    Inspection r; r.reason="corruptPE"; IMAGE_NT_HEADERS64 nt{}; size_t at=0;
    if(!headers(b,nt,at) || !nt.FileHeader.NumberOfSections) return r;
    const size_t sections=at+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
    if(!range(sections,static_cast<size_t>(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER),b.size())) return r;
    auto fileOffset=[&](DWORD rva,size_t length)->std::optional<size_t> {
        if(rva<nt.OptionalHeader.SizeOfHeaders && range(rva,length,b.size()) && length<=nt.OptionalHeader.SizeOfHeaders-rva) return rva;
        for(size_t i=0;i<nt.FileHeader.NumberOfSections;++i) {
            IMAGE_SECTION_HEADER s{}; if(!read(b,sections+i*sizeof(s),s)) return {};
            if(rva>=s.VirtualAddress && rva-s.VirtualAddress<s.SizeOfRawData) {
                size_t off=static_cast<size_t>(s.PointerToRawData)+(rva-s.VirtualAddress);
                if(length<=s.SizeOfRawData-(rva-s.VirtualAddress) && range(off,length,b.size())) return off;
            }
        } return {};
    };
    const auto dir=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    auto exp=fileOffset(dir.VirtualAddress,sizeof(IMAGE_EXPORT_DIRECTORY)); IMAGE_EXPORT_DIRECTORY e{};
    if(!dir.VirtualAddress || dir.Size<sizeof(e) || !exp || !read(b,*exp,e)) return r;
    auto names=fileOffset(e.AddressOfNames,static_cast<size_t>(e.NumberOfNames)*sizeof(DWORD));
    auto ords=fileOffset(e.AddressOfNameOrdinals,static_cast<size_t>(e.NumberOfNames)*sizeof(WORD));
    auto funcs=fileOffset(e.AddressOfFunctions,static_cast<size_t>(e.NumberOfFunctions)*sizeof(DWORD));
    if(!names || !ords || !funcs) return r;
    for(size_t i=0;i<e.NumberOfNames;++i) {
        DWORD name=0,fn=0; WORD ordinal=0;
        if(!read(b,*names+i*4,name) || !read(b,*ords+i*2,ordinal) || ordinal>=e.NumberOfFunctions || !read(b,*funcs+ordinal*4,fn)) return r;
        auto text=fileOffset(name,1); if(!text) return r;
        std::string value;
        for(size_t k=0;k<256;++k) {
            if(name>UINT32_MAX-k) return r;
            auto ch=fileOffset(name+static_cast<DWORD>(k),1); if(!ch) return r;
            if(!b[*ch]) break; value+=static_cast<char>(b[*ch]);
        }
        if(value.size()==256) return r;
        if(std::find(requiredNames.begin(),requiredNames.end(),value)!=requiredNames.end()) {
            if(!fn || (fn>=dir.VirtualAddress && static_cast<uint64_t>(fn)-dir.VirtualAddress<dir.Size) || !fileOffset(fn,1)) {
                r.reason="forwardedOrInvalidExport"; return r;
            }
        }
        r.exports.push_back(std::move(value));
    }
    for(auto name:requiredNames) if(std::find(r.exports.begin(),r.exports.end(),name)==r.exports.end()) { r.reason="missingExport"; return r; }
    r.accepted=true; r.reason="accepted"; return r;
}
Inspection inspect_pe(std::span<const unsigned char> b) { return inspect_named_pe(b,required); }
Inspection inspect_driver_pe(std::span<const unsigned char> b) {
    constexpr const char* names[]{"NVSDK_NGX_D3D12_Init_Ext","NVSDK_NGX_D3D12_CreateFeature",
        "NVSDK_NGX_D3D12_EvaluateFeature","NVSDK_NGX_D3D12_ReleaseFeature","NVSDK_NGX_D3D12_Shutdown1",
        "NVSDK_NGX_D3D12_GetCapabilityParameters","NVSDK_NGX_D3D12_DestroyParameters"};
    return inspect_named_pe(b,names);
}
Inspection inspect(const RuntimeIdentity& id) {
    Inspection r;
    try {
        if(!id.canonicalPath.is_absolute() || id.sha256.size()!=64 || std::filesystem::canonical(id.canonicalPath)!=id.canonicalPath.lexically_normal()) {
            r.reason="nonCanonicalPath"; return r;
        }
        File file; file.handle=CreateFileW(id.canonicalPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file.handle==INVALID_HANDLE_VALUE) { r.reason="runtimeOpenFailed"; return r; }
        auto b=bytes(file.handle); r=inspect_pe(b); r.sha256=digest(b);
        if(r.sha256!=id.sha256) { r.accepted=false; r.reason="hashMismatch"; }
    } catch(const std::exception& e) { r.accepted=false; r.reason=e.what(); }
    return r;
}
namespace {
struct Key {
    HKEY value=nullptr;
    ~Key(){if(value&&value!=reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE))RegCloseKey(value);}
};
std::vector<wchar_t> registry_value(HKEY key,const wchar_t* name,DWORD expected) {
    DWORD type=0,size=0;
    if(RegQueryValueExW(key,name,nullptr,&type,nullptr,&size)!=ERROR_SUCCESS||type!=expected||
        size<sizeof(wchar_t)||size>32768||size%sizeof(wchar_t))throw std::runtime_error("active driver registry value type/extent");
    std::vector<wchar_t> value(size/sizeof(wchar_t));
    if(RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(value.data()),&size)!=ERROR_SUCCESS||
        type!=expected||size!=value.size()*sizeof(wchar_t)||value.back()!=0)
        throw std::runtime_error("active driver registry value changed/unbounded");
    return value;
}
std::wstring registry_string(HKEY key,const wchar_t* name) {
    const auto value=registry_value(key,name,REG_SZ);
    if(value.size()<2||std::find(value.begin(),value.end()-1,L'\0')!=value.end()-1)
        throw std::runtime_error("active driver registry string missing/ambiguous");
    return {value.data(),value.size()-1};
}
bool component(const std::wstring& value) {
    return !value.empty()&&value!=L"."&&value!=L".."&&
        std::none_of(value.begin(),value.end(),[](wchar_t c){return c<32||c==L'/'||c==L'\\'||c==L':';});
}
}
DriverRegistration driver_registration(std::span<const DriverRegistration> candidates) {
    if(candidates.size()!=1)throw std::runtime_error("active NVIDIA driver missing/ambiguous");
    const auto& value=candidates.front();
    unsigned parts=0,number=0,digits=0;
    bool versionValid=!value.driverVersion.empty();
    for(const auto c:value.driverVersion) {
        if(c==L'.') {versionValid=versionValid&&digits>0;++parts;number=digits=0;}
        else if(c>=L'0'&&c<=L'9') {
            number=number*10+static_cast<unsigned>(c-L'0');++digits;
            if(number>65535||digits>5)versionValid=false;
        }else versionValid=false;
    }
    versionValid=versionValid&&parts==3&&digits>0;
    if(value.deviceInstance.size()<13||_wcsnicmp(value.deviceInstance.c_str(),L"PCI\\VEN_10DE&",13)!=0||
        value.driverKey.empty()||!component(value.publishedInf)||!component(value.activePackage)||
        std::filesystem::path(value.publishedInf).extension()!=L".inf"||!component(value.infSection)||
        !versionValid)
        throw std::runtime_error("active NVIDIA driver registration unbound");
    return value;
}
std::filesystem::path published_inf_path(const std::filesystem::path& directory,std::wstring_view name) {
    const std::wstring value(name);
    if(!directory.is_absolute()||!component(value)||std::filesystem::path(value).extension()!=L".inf")
        throw std::runtime_error("registered published INF name unbound");
    return directory/value;
}
DriverBinding resolve_driver() {
    constexpr GUID display{0x4d36e968,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
    struct Devices {
        HDEVINFO value=INVALID_HANDLE_VALUE;
        ~Devices(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}
    } devices;
    devices.value=SetupDiGetClassDevsW(&display,nullptr,nullptr,DIGCF_PRESENT);
    if(devices.value==INVALID_HANDLE_VALUE)throw std::runtime_error("present display registration unavailable");
    std::vector<DriverRegistration> candidates;
    for(DWORD index=0;;++index) {
        SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiEnumDeviceInfo(devices.value,index,&device)) {
            if(GetLastError()==ERROR_NO_MORE_ITEMS)break;
            throw std::runtime_error("present display registration enumeration failed");
        }
        DWORD type=0,size=0;
        SetupDiGetDeviceRegistryPropertyW(devices.value,&device,SPDRP_HARDWAREID,&type,nullptr,0,&size);
        if(!size)continue;
        if(type!=REG_MULTI_SZ||size>32768||size%sizeof(wchar_t))
            throw std::runtime_error("display hardware registration extent");
        std::vector<wchar_t> hardware(size/sizeof(wchar_t));
        if(!SetupDiGetDeviceRegistryPropertyW(devices.value,&device,SPDRP_HARDWAREID,&type,
            reinterpret_cast<BYTE*>(hardware.data()),size,nullptr)||hardware.back()!=0)
            throw std::runtime_error("display hardware registration read");
        if(hardware.size()<13||_wcsnicmp(hardware.data(),L"PCI\\VEN_10DE&",13)!=0)continue;
        ULONG status=0,problem=0;
        if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)!=CR_SUCCESS||!(status&DN_STARTED)||problem)
            continue;
        std::array<wchar_t,MAX_DEVICE_ID_LEN> instance{};
        if(!SetupDiGetDeviceInstanceIdW(devices.value,&device,instance.data(),static_cast<DWORD>(instance.size()),nullptr))
            throw std::runtime_error("active NVIDIA device instance missing");
        DWORD driverType=0,driverSize=0;
        SetupDiGetDeviceRegistryPropertyW(devices.value,&device,SPDRP_DRIVER,&driverType,nullptr,0,&driverSize);
        if(driverType!=REG_SZ||!driverSize||driverSize>32768||driverSize%sizeof(wchar_t))
            throw std::runtime_error("active NVIDIA class binding missing");
        std::vector<wchar_t> driver(driverSize/sizeof(wchar_t));
        if(!SetupDiGetDeviceRegistryPropertyW(devices.value,&device,SPDRP_DRIVER,&driverType,
            reinterpret_cast<BYTE*>(driver.data()),driverSize,nullptr)||driver.back()!=0)
            throw std::runtime_error("active NVIDIA class binding read");
        Key key;key.value=SetupDiOpenDevRegKey(devices.value,&device,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_READ);
        if(key.value==reinterpret_cast<HKEY>(INVALID_HANDLE_VALUE))throw std::runtime_error("active NVIDIA driver class key unavailable");
        DriverRegistration entry{instance.data(),driver.data(),registry_string(key.value,L"InfPath"),{},
            registry_string(key.value,L"DriverVersion"),registry_string(key.value,L"InfSection")};
        if(!component(entry.publishedInf))throw std::runtime_error("active NVIDIA published INF unbound");
        std::array<wchar_t,32768> windows{},store{},published{};
        const auto windowsCount=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));
        if(!windowsCount||windowsCount>=windows.size())throw std::runtime_error("registered Windows INF directory missing");
        const auto inf=std::filesystem::canonical(published_inf_path(std::filesystem::path(windows.data())/L"INF",entry.publishedInf));
        if(!SetupGetInfDriverStoreLocationW(inf.c_str(),nullptr,nullptr,store.data(),static_cast<DWORD>(store.size()),nullptr))
            throw std::runtime_error("registered NVIDIA INF to DriverStore mapping failed Win32="+std::to_string(GetLastError()));
        const auto storeInf=std::filesystem::canonical(store.data());
        if(!SetupGetInfPublishedNameW(storeInf.c_str(),published.data(),static_cast<DWORD>(published.size()),nullptr)||
            std::filesystem::canonical(published_inf_path(std::filesystem::path(windows.data())/L"INF",published.data()))!=inf)
            throw std::runtime_error("registered NVIDIA INF/DriverStore roundtrip unbound");
        entry.activePackage=storeInf.parent_path().filename().wstring();
        candidates.push_back(std::move(entry));
    }
    DriverBinding result;result.registration=driver_registration(candidates);
    std::array<wchar_t,32768> system{};const auto count=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
    if(!count||count>=system.size())throw std::runtime_error("canonical system directory missing");
    const auto repository=std::filesystem::canonical(std::filesystem::path(system.data())/L"DriverStore"/L"FileRepository");
    const auto directory=std::filesystem::canonical(repository/result.registration.activePackage);
    if(directory.parent_path()!=repository)throw std::runtime_error("active NVIDIA package escaped DriverStore");
    std::array<wchar_t,32768> store{};
    const auto publishedInf=std::filesystem::canonical(published_inf_path(std::filesystem::path(system.data()).parent_path()/
        L"INF",result.registration.publishedInf));
    if(!SetupGetInfDriverStoreLocationW(publishedInf.c_str(),nullptr,nullptr,
        store.data(),static_cast<DWORD>(store.size()),nullptr))throw std::runtime_error("fresh registered NVIDIA INF mapping failed");
    result.driverStoreInf=std::filesystem::canonical(store.data());
    if(result.driverStoreInf.parent_path()!=directory)throw std::runtime_error("fresh NVIDIA package registration changed");
    result.identity.canonicalPath=std::filesystem::canonical(directory/L"_nvngx.dll");
    if(result.identity.canonicalPath.parent_path()!=directory)throw std::runtime_error("active NVIDIA CORE unbound");
    File file;file.handle=CreateFileW(result.identity.canonicalPath.c_str(),GENERIC_READ,FILE_SHARE_READ,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file.handle==INVALID_HANDLE_VALUE)throw std::runtime_error("active NVIDIA CORE open failed");
    const auto data=bytes(file.handle);const auto inspected=inspect_driver_pe(data);
    if(!inspected.accepted)throw std::runtime_error("active NVIDIA CORE PE/exports refused: "+inspected.reason);
    result.identity.sha256=digest(data);
    DWORD ignored=0;const auto versionSize=GetFileVersionInfoSizeW(result.identity.canonicalPath.c_str(),&ignored);
    if(!versionSize||versionSize>1048576)throw std::runtime_error("active NVIDIA CORE version missing");
    std::vector<unsigned char> version(versionSize);
    if(!GetFileVersionInfoW(result.identity.canonicalPath.c_str(),0,versionSize,version.data()))
        throw std::runtime_error("active NVIDIA CORE version read");
    VS_FIXEDFILEINFO* fixed=nullptr;UINT fixedBytes=0;
    if(!VerQueryValueW(version.data(),L"\\",reinterpret_cast<void**>(&fixed),&fixedBytes)||
        fixedBytes<sizeof(*fixed)||fixed->dwSignature!=0xfeef04bd)
        throw std::runtime_error("active NVIDIA CORE version invalid");
    const auto actualVersion=std::to_wstring(HIWORD(fixed->dwFileVersionMS))+L"."+
        std::to_wstring(LOWORD(fixed->dwFileVersionMS))+L"."+std::to_wstring(HIWORD(fixed->dwFileVersionLS))+L"."+
        std::to_wstring(LOWORD(fixed->dwFileVersionLS));
    if(actualVersion!=result.registration.driverVersion)throw std::runtime_error("active NVIDIA CORE/class version mismatch");
    WINTRUST_FILE_INFO trusted{};trusted.cbStruct=sizeof(trusted);
    trusted.pcwszFilePath=result.identity.canonicalPath.c_str();trusted.hFile=file.handle;
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&trusted;
    trust.dwStateAction=WTD_STATEACTION_VERIFY;
    trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto accepted=WinVerifyTrust(nullptr,&action,&trust);
    trust.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&action,&trust);
    if(accepted!=ERROR_SUCCESS)throw std::runtime_error("active NVIDIA CORE signature refused");
    result.signatureVerified=true;return result;
}
// R2 named imports: loaded-image RVAs, never version-specific fixed addresses.
bool find_path_imports(std::span<unsigned char> image,std::vector<ImportSlot>& slots) {
    slots.clear(); IMAGE_NT_HEADERS64 nt{}; size_t at=0;
    if(!headers(image,nt,at)) return false;
    const auto d=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!d.VirtualAddress || !range(d.VirtualAddress,d.Size,image.size())) return false;
    std::vector<ImportSlot> found;
    for(size_t off=0;off+sizeof(IMAGE_IMPORT_DESCRIPTOR)<=d.Size;off+=sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR desc{}; if(!read(image,static_cast<size_t>(d.VirtualAddress)+off,desc)) return false;
        if(!desc.Name && !desc.FirstThunk && !desc.OriginalFirstThunk) { slots=std::move(found); return true; }
        if(!desc.FirstThunk) return false;
        if(!desc.OriginalFirstThunk) continue;
        for(size_t i=0;;++i) {
            const size_t n=static_cast<size_t>(desc.OriginalFirstThunk)+i*sizeof(IMAGE_THUNK_DATA64);
            const size_t s=static_cast<size_t>(desc.FirstThunk)+i*sizeof(IMAGE_THUNK_DATA64);
            IMAGE_THUNK_DATA64 entry{};
            if(!read(image,n,entry) || !range(s,sizeof(entry),image.size())) return false;
            if(!entry.u1.AddressOfData) break;
            if(IMAGE_SNAP_BY_ORDINAL64(entry.u1.AddressOfData)) continue;
            const auto nameAt=entry.u1.AddressOfData;
            if(!range(nameAt,sizeof(WORD)+1,image.size())) return false;
            const char* name=reinterpret_cast<const char*>(image.data()+nameAt+sizeof(WORD));
            const char* end=static_cast<const char*>(memchr(name,0,image.size()-nameAt-sizeof(WORD)));
            if(!end) return false;
            const std::string_view imported(name,static_cast<size_t>(end-name));
            if(imported=="GetModuleFileNameW" || imported=="GetModuleFileNameA") {
                if(s%alignof(void*)) return false;
                found.push_back({reinterpret_cast<void**>(image.data()+s),imported.back()=='W'});
            }
        }
    } return false;
}
namespace {
// Thread-local call context only, never an owner/runtime registry.
thread_local HMODULE callerAlias=nullptr;
DWORD WINAPI callerW(HMODULE queried,LPWSTR path,DWORD capacity) {
    if(callerAlias && queried==callerAlias) {
        constexpr wchar_t alias[]=L"nvngx.dll";
        if(!capacity) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        auto n=static_cast<DWORD>(std::min<size_t>(capacity-1,std::size(alias)-1));
        memcpy(path,alias,n*sizeof(wchar_t)); path[n]=0;
        if(capacity<std::size(alias)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return capacity; } return n;
    } return GetModuleFileNameW(queried,path,capacity);
}
DWORD WINAPI callerA(HMODULE queried,LPSTR path,DWORD capacity) {
    if(callerAlias && queried==callerAlias) {
        constexpr char alias[]="nvngx.dll";
        if(!capacity) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        auto n=static_cast<DWORD>(std::min<size_t>(capacity-1,std::size(alias)-1));
        memcpy(path,alias,n); path[n]=0;
        if(capacity<std::size(alias)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return capacity; } return n;
    } return GetModuleFileNameA(queried,path,capacity);
}
struct CallerScope {
    HMODULE previous=callerAlias;
    explicit CallerScope(bool direct) {
        if(direct && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&callerW),&callerAlias)) throw std::runtime_error("caller alias");
    }
    ~CallerScope() { callerAlias=previous; }
};
bool replace(void** slot,void* expected,void* value) {
    if(*slot!=expected) return false;
    DWORD protection=0;
    if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)) return false;
    const bool done=InterlockedCompareExchangePointer(slot,value,expected)==expected; DWORD ignored=0;
    if(!VirtualProtect(slot,sizeof(void*),protection,&ignored)) return false;
    return done;
}
}
struct Module::Impl {
    lifecycle::VendorTrace* trace=nullptr; // Session-owned; the explicit SDK owner retains its full lifetime.
    Dispatch driver,direct; Route route=Route::Driver;
    std::shared_ptr<void> lease; RuntimeIdentity identity;
    ComPtr<ID3D12Device> device;
    File pinnedFile; HMODULE handle=nullptr;
    std::filesystem::path dataPath;
    std::mutex mutex;
    struct Binding { ImportSlot slot; void* original; };
    std::vector<Binding> imports;
    using Init=NVSDK_NGX_Result (*)(unsigned long long,const wchar_t*,ID3D12Device*,unsigned,NVSDK_NGX_Parameter*);
    Init init=nullptr;
    decltype(&NVSDK_NGX_D3D12_Shutdown1) shutdown=nullptr;
    NVSDK_NGX_Parameter* capabilities=nullptr;
    bool initialized=false,controlled=false,attempted=false;
    bool initUnreturned=false,initFailed=false;
    RetirementControl retirement;
    std::function<bool()> closeBoundary;
    bool closed=false;
    unsigned passOwners=0;
    template<class Invoke> lifecycle::VendorResult traced(lifecycle::VendorStage stage,uintptr_t module,uintptr_t procedure,Invoke&& invoke) {
        if(trace)return trace->call(stage,{module,procedure,GetCurrentThreadId()},std::forward<Invoke>(invoke));
        return invoke();
    }
    bool openDirect() {
        if(direct.valid()) return true;
        if(controlled || attempted) return false;
        attempted=true;
        // No shared IAT ownership: caller explicitly shares Module between native passes.
        HMODULE existing=nullptr;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,identity.canonicalPath.c_str(),&existing)) return false;
        handle=LoadLibraryExW(identity.canonicalPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!handle) return false;
        auto symbol=[&](const char* name) { return GetProcAddress(handle,name); };
        init=reinterpret_cast<Init>(symbol(required[0]));
        Dispatch candidate{driver.allocate,driver.destroy,reinterpret_cast<decltype(direct.create)>(symbol(required[1])),
            reinterpret_cast<decltype(direct.evaluate)>(symbol(required[2])),reinterpret_cast<decltype(direct.release)>(symbol(required[3]))};
        shutdown=reinterpret_cast<decltype(shutdown)>(symbol(required[4]));
        if(!init || !shutdown || !candidate.valid()) return false;
        MODULEINFO image{}; std::vector<ImportSlot> slots;
        if(!GetModuleInformation(GetCurrentProcess(),handle,&image,sizeof(image)) ||
            !find_path_imports({static_cast<unsigned char*>(image.lpBaseOfDll),image.SizeOfImage},slots)) return false;
        for(auto slot:slots) {
            void* original=slot.wide?reinterpret_cast<void*>(&GetModuleFileNameW):reinterpret_cast<void*>(&GetModuleFileNameA);
            if(*slot.address!=original) return false; // refuse another owner/wrapper
        }
        for(auto slot:slots) {
            void* original=*slot.address; void* shim=slot.wide?reinterpret_cast<void*>(&callerW):reinterpret_cast<void*>(&callerA);
            imports.push_back({slot,original});
            if(!replace(slot.address,original,shim)) return false;
        }
        if(driver.allocate(&capabilities)!=NVSDK_NGX_Result_Success || !capabilities) return false;
        CallerScope call(true);
        initUnreturned=true;
        const auto receipt=traced(lifecycle::VendorStage::NrInit,reinterpret_cast<uintptr_t>(handle),reinterpret_cast<uintptr_t>(init),[&] {
            return lifecycle::VendorResult{static_cast<uint32_t>(init(0x24480451ull,dataPath.c_str(),device.Get(),0x15,capabilities)),0,0};
        });
        initUnreturned=false;
        initialized=receipt.result==static_cast<uint32_t>(NVSDK_NGX_Result_Success);
        initFailed=!initialized;
        if(initialized) direct=candidate;
        return initialized;
    }
    bool close() {
        if(closed)return true;
        if(passOwners)return false;
        if(initUnreturned || initFailed)return false;
        if(closeBoundary && !closeBoundary())return false;
        CallerScope call(initialized && !controlled);
        if(initialized) {
            const auto receipt=traced(lifecycle::VendorStage::NrShutdown,reinterpret_cast<uintptr_t>(handle),reinterpret_cast<uintptr_t>(shutdown),[&] {
                return lifecycle::VendorResult{static_cast<uint32_t>(controlled?retirement.shutdown():shutdown(device.Get())),0,0};
            });
            if(receipt.result!=static_cast<uint32_t>(NVSDK_NGX_Result_Success))return false;
            initialized=false;
        }
        if(capabilities) {
            const auto receipt=traced(lifecycle::VendorStage::CapabilityDestroy,
                trace?trace->snapshot().calls[static_cast<size_t>(lifecycle::VendorStage::CoreInit)].first.module:0,
                reinterpret_cast<uintptr_t>(driver.destroy),[&] {
                    return lifecycle::VendorResult{static_cast<uint32_t>(driver.destroy(capabilities)),0,0};
                });
            if(receipt.result!=static_cast<uint32_t>(NVSDK_NGX_Result_Success))return false;
            capabilities=nullptr;
        }
        for(auto& b:imports) {
            auto shim=b.slot.wide?reinterpret_cast<void*>(&callerW):reinterpret_cast<void*>(&callerA);
            if(*b.slot.address==b.original) continue;
            if(!replace(b.slot.address,shim,b.original)) return false;
        }
        imports.clear();
        if(retirement.restoreImports) {
            if(!retirement.restoreImports())return false;
            retirement.restoreImports={};
        }
        if(handle || retirement.releaseLibrary) {
            const auto receipt=traced(lifecycle::VendorStage::NrLibraryRelease,reinterpret_cast<uintptr_t>(handle),reinterpret_cast<uintptr_t>(&FreeLibrary),[&] {
                if(retirement.releaseLibrary)return retirement.releaseLibrary();
                const auto result=FreeLibrary(handle);
                return lifecycle::VendorResult{static_cast<uint32_t>(result),0,result?0u:GetLastError()};
            });
            if(!receipt.result)return false;
            retirement.releaseLibrary={};
        }
        handle=nullptr;closed=true;return true;
    }
};
Module::Module(std::unique_ptr<Impl> p):impl_(std::move(p)) {}
bool Module::close() {
    std::lock_guard lock(impl_->mutex);
    return impl_->close();
}
Module::~Module() {
    if(!impl_->close()) {
        OutputDebugStringA("NR runtime: shutdown/import restoration failed; retaining owned module/resources\n");
        (void)impl_.release();
    }
}
std::shared_ptr<Module> Module::controlled(Dispatch driver,Dispatch direct,std::shared_ptr<void> lease,Route route,
    std::function<bool()> closeBoundary,RetirementControl retirement) {
    if(!driver.valid() || !lease) return {};
    auto p=std::make_unique<Impl>(); p->driver=driver; p->direct=direct; p->lease=std::move(lease); p->route=route; p->controlled=true;
    p->closeBoundary=std::move(closeBoundary);
    p->trace=retirement.trace;p->capabilities=retirement.capabilities;
    p->initialized=static_cast<bool>(retirement.shutdown);p->retirement=std::move(retirement);
    return std::shared_ptr<Module>(new Module(std::move(p)));
}
std::shared_ptr<Module> Module::open(const RuntimeIdentity& id,Dispatch driver,std::shared_ptr<void> lease,
    ID3D12Device* device,const std::filesystem::path& dataPath,Route route,lifecycle::VendorTrace* trace) {
    if(!driver.valid() || !lease || !device || !dataPath.is_absolute() || !std::filesystem::is_directory(dataPath)) return {};
    auto p=std::make_unique<Impl>();
    p->trace=trace;
    p->pinnedFile.handle=CreateFileW(id.canonicalPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(p->pinnedFile.handle==INVALID_HANDLE_VALUE || !inspect(id).accepted) return {};
    p->identity=id; p->driver=driver; p->lease=std::move(lease); p->device=device; p->dataPath=dataPath; p->route=route;
    auto owner=std::shared_ptr<Module>(new Module(std::move(p)));
    if(route==Route::Direct && !owner->impl_->openDirect()) return {};
    return owner;
}
struct Submission::Impl {
    Completion status=Completion::Recorded;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
};
Submission::Submission():impl_(std::make_unique<Impl>()) {}
Submission::~Submission() { if(state()==Completion::Submitted || state()==Completion::SignalFailed) (void)impl_.release(); }
bool Submission::submitted(ID3D12CommandQueue* queue) {
    if(impl_->status!=Completion::Recorded || !queue) return false;
    impl_->queue=queue;
    impl_->status=Completion::SignalFailed; // ExecuteCommandLists already happened, ANY failure is unresolved
    ComPtr<ID3D12Device> device;
    if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&impl_->fence))) ||
        FAILED(queue->Signal(impl_->fence.Get(),1))) return false;
    impl_->status=Completion::Submitted; return true;
}
bool Submission::discard() {
    if(impl_->status!=Completion::Recorded) return false;
    impl_->status=Completion::Discarded; return true;
}
Completion Submission::state() const {
    if(impl_->status==Completion::Submitted) {
        const auto value=impl_->fence->GetCompletedValue();
        if(value==UINT64_MAX) return Completion::SignalFailed; // removed device is NOT completion
        if(value>=1) return Completion::Complete;
    } return impl_->status;
}
namespace {
void tuning(NVSDK_NGX_Parameter* p,const ModelTuning& s) {
    p->Set("DLSSNR.Intensity",s.intensity); p->Set("DLSSNR.Style",s.style);
    p->Set("DLSSNR.LocalStructureStrength",s.localStructure); p->Set("DLSSNR.LocalToneStrength",s.localTone);
    p->Set("DLSSNR.SkinStructureStrength",s.skinStructure); p->Set("DLSSNR.UseAutoMask",s.autoMask?1u:0u);
}
void region(NVSDK_NGX_Parameter* p,const char* prefix,Rect r) {
    std::string name=std::string("DLSSNR.")+prefix+"Subrect";
    p->Set((name+"BaseX").c_str(),r.x); p->Set((name+"BaseY").c_str(),r.y);
    p->Set((name+"Width").c_str(),r.width); p->Set((name+"Height").c_str(),r.height);
}
bool settled(const Recording& r) {
    const auto s=r.completion(); return s==Completion::Complete || s==Completion::Discarded;
}
bool valid(const Recording& r) { return r.commands && r.epoch && r.resourceLease && r.completion; }
}
struct Pass::Impl {
    std::shared_ptr<Module> module;
    NVSDK_NGX_Parameter* params=nullptr;
    NVSDK_NGX_Handle* feature=nullptr;
    std::optional<Settings> settings;
    std::optional<Extent> extent;
    uint64_t history=0,creationEpoch=0,serial=0;
    std::optional<Recording> creation,evaluation;
    std::optional<JobContract> job;
    PublicationState publication=PublicationState::Pending;
    bool failed=false,retired=false,direct=false,reset=true,evaluated=false,producerComplete=false;
    explicit Impl(std::shared_ptr<Module> m):module(std::move(m)) {}
    Dispatch& api() { return direct?module->impl_->direct:module->impl_->driver; }
};
Pass::Pass(std::shared_ptr<Module> m):impl_(std::make_unique<Impl>(std::move(m))) {
    if(!impl_->module) throw std::invalid_argument("runtime module required");
    std::lock_guard lock(impl_->module->impl_->mutex);
    if(impl_->module->impl_->closed)throw std::invalid_argument("runtime module closed");
    ++impl_->module->impl_->passOwners;
}
Pass::~Pass() {
    if(!release()) {
        OutputDebugStringA("NR runtime: unresolved or failed feature teardown; retaining pass/module/resources\n");
        (void)impl_.release();
    }else {
        std::lock_guard lock(impl_->module->impl_->mutex);
        --impl_->module->impl_->passOwners;
    }
}
NVSDK_NGX_Result Pass::prepare(const Settings& settings,Extent size,uint64_t history,Recording recording) {
    auto& p=*impl_; auto& m=*p.module->impl_; std::lock_guard lock(m.mutex);
    if(p.failed || p.retired || !settings.valid() || !settings.enabled || !size.width || !size.height || !history || !valid(recording) ||
        (!m.controlled && std::filesystem::path(settings.runtimePath)!=m.identity.canonicalPath)) return NVSDK_NGX_Result_Fail;
    if(p.params) return p.settings==settings && p.extent==size && p.history==history?NVSDK_NGX_Result_Success:NVSDK_NGX_Result_Fail;
    p.settings.emplace(settings); p.extent.emplace(size); p.history=history;
    auto result=m.driver.allocate(&p.params);
    if(result!=NVSDK_NGX_Result_Success || !p.params) { p.failed=true; return NVSDK_NGX_Result_Fail; }
    auto* params=p.params;
    params->Set("DLSSNR.Enabled",1u); params->Set("DLSSNR.Width",size.width); params->Set("DLSSNR.Height",size.height);
    params->Set("CreationNodeMask",1u); params->Set("VisibilityNodeMask",1u);
    params->Set("DLSSNR.Hint.Render.Preset",settings.tuning.preset); tuning(params,settings.tuning);
    params->Set("DLSSNR.UICorrection",1u);
    for(auto name:{"DLSSNR.ControlMask","DLSSNR.UI","DLSSNR.UIAlpha","DLSSNR.Backbuffer"})
        params->Set(name,static_cast<ID3D12Resource*>(nullptr));
    region(params,"UI",{0,0,0,0}); region(params,"UIAlpha",{0,0,0,0}); region(params,"Backbuffer",{0,0,0,0});
    p.creationEpoch=recording.epoch; p.creation.emplace(std::move(recording)); // retain before foreign call, even failure
    p.direct=m.route==Route::Direct;
    if(p.direct && !m.openDirect()) { p.failed=true; return NVSDK_NGX_Result_Fail; }
    {
        CallerScope scope(p.direct && !m.controlled);
        result=p.api().create(p.creation->commands,static_cast<NVSDK_NGX_Feature>(18),params,&p.feature);
    }
    if(result!=NVSDK_NGX_Result_Success && !p.feature && m.route==Route::DriverFallback && m.openDirect()) {
        p.direct=true; CallerScope scope(!m.controlled);
        result=p.api().create(p.creation->commands,static_cast<NVSDK_NGX_Feature>(18),params,&p.feature);
    }
    if(result!=NVSDK_NGX_Result_Success || !p.feature) { p.failed=true; return NVSDK_NGX_Result_Fail; }
    return result;
}
bool Pass::ready(uint64_t epoch) const {
    auto& p=*impl_;
    if(p.creation && p.creation->completion()==Completion::SignalFailed) p.failed=true;
    return p.feature && !p.failed && !p.retired && p.creation && epoch>p.creationEpoch &&
        p.creation->completion()==Completion::Complete;
}
NVSDK_NGX_Result Pass::run(const JobContract& job,const Inputs& input,Recording recording) {
    auto& p=*impl_; auto& m=*p.module->impl_; std::lock_guard lock(m.mutex);
    const Epochs epochs{job.key.frame.instanceId,job.key.frame.contentEpoch,job.key.historyEpoch,job.key.resourceGeneration};
    if(!valid(recording) || !ready(recording.epoch) || !job.key.valid() || job.key.frame!=job.input.key ||
        input_problem(job.input,epochs)!=Reason::None || job.input.settings!=*p.settings ||
        job.input.descriptor.active.width!=p.extent->width || job.input.descriptor.active.height!=p.extent->height ||
        job.key.submissionSerial<=p.serial || job.key.historyEpoch<p.history ||
        (p.job && (job.key.frame.instanceId!=p.job->key.frame.instanceId ||
            job.key.resourceGeneration!=p.job->key.resourceGeneration || job.key.frame.contentEpoch<p.job->key.frame.contentEpoch ||
            (job.key.frame.contentEpoch!=p.job->key.frame.contentEpoch && job.key.historyEpoch<=p.history))) ||
        !input.color || !input.depth || !input.motion || !input.output || !input.producerComplete ||
        !input.depthRegion.width || !input.depthRegion.height || !input.motionRegion.width || !input.motionRegion.height ||
        !std::isfinite(input.mvScaleX) || !std::isfinite(input.mvScaleY) ||
        (p.evaluation && !settled(*p.evaluation))) return NVSDK_NGX_Result_Fail;
    if(job.key.historyEpoch!=p.history || (p.evaluation && p.evaluation->completion()==Completion::Discarded)) {
        p.history=job.key.historyEpoch; p.reset=true;
    }
    p.job.emplace(job); p.serial=job.key.submissionSerial; p.publication=PublicationState::Pending;
    p.evaluated=false; p.producerComplete=input.producerComplete;
    p.evaluation.emplace(std::move(recording));
    auto* params=p.params;
    params->Set("DLSSNR.Color",input.color); params->Set("DLSSNR.Depth",input.depth);
    params->Set("DLSSNR.MVec",input.motion); params->Set("DLSSNR.Output",input.output);
    params->Set("DLSSNR.Enabled",1u); params->Set("DLSSNR.Width",p.extent->width); params->Set("DLSSNR.Height",p.extent->height);
    params->Set("DLSSNR.DepthInverted",input.depthInverted?1u:0u); params->Set("DLSSNR.Reset",(input.reset||p.reset)?1u:0u);
    region(params,"Color",{0,0,p.extent->width,p.extent->height}); region(params,"Output",{0,0,p.extent->width,p.extent->height});
    region(params,"Depth",input.depthRegion); region(params,"MVec",input.motionRegion);
    params->Set("DLSSNR.MVecScaleX",input.mvScaleX); params->Set("DLSSNR.MVecScaleY",input.mvScaleY); tuning(params,p.settings->tuning);
    CallerScope scope(p.direct && !m.controlled);
    const auto result=p.api().evaluate(p.evaluation->commands,p.feature,params,nullptr);
    p.evaluated=result==NVSDK_NGX_Result_Success; p.failed=!p.evaluated;
    if(p.evaluated) p.reset=false;
    return result;
}
std::optional<CompletedFrame> Pass::completed(const JobKey& key) const {
    auto& p=*impl_;
    if(p.evaluation && p.evaluation->completion()==Completion::SignalFailed) { p.failed=true; p.evaluated=false; }
    if(!p.job || p.job->key!=key || !p.evaluation || p.evaluation->completion()!=Completion::Complete) return {};
    auto publication=p.publication==PublicationState::Pending?PublicationState::Completed:p.publication;
    return CompletedFrame{*p.job,publication,p.producerComplete,true,p.evaluated};
}
bool Pass::publishable(const JobKey& key) const {
    auto c=completed(key); return !impl_->retired && !impl_->failed && c && c->evaluated && c->publication==PublicationState::Completed;
}
void Pass::expire(const JobKey& key) {
    if(impl_->job && impl_->job->key==key) if(auto s=advance_publication(impl_->publication,PublicationEvent::Expire)) impl_->publication=*s;
}
void Pass::cancel(const JobKey& key) {
    if(impl_->job && impl_->job->key==key) if(auto s=advance_publication(impl_->publication,PublicationEvent::Cancel)) impl_->publication=*s;
}
bool Pass::release() {
    auto& p=*impl_; auto& m=*p.module->impl_; std::lock_guard lock(m.mutex); p.retired=true;
    if((p.creation && !settled(*p.creation)) || (p.evaluation && !settled(*p.evaluation))) return false;
    CallerScope scope(p.direct && !m.controlled);
    if(p.feature) { if(p.api().release(p.feature)!=NVSDK_NGX_Result_Success) return false; p.feature=nullptr; }
    if(p.params) { if(m.driver.destroy(p.params)!=NVSDK_NGX_Result_Success) return false; p.params=nullptr; }
    p.creation.reset(); p.evaluation.reset(); return true;
}
}
