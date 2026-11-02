// SPDX-License-Identifier: GPL-3.0-only
// Production adapter; doubles replace only NGX ABI and physical completion boundaries.
#include "nr_runtime.h"
#include <cstdio>
#include <map>
#include <stdexcept>
#include <variant>
struct NVSDK_NGX_Handle { uint64_t token=18; }; // opaque ABI handle owned by the NGX boundary double
using namespace nr;
using namespace nr::runtime;
static unsigned checks=0;
static void check(bool v) { ++checks; if(!v) throw std::runtime_error("runtime assertion"); }
struct Params final : NVSDK_NGX_Parameter {
    using Value=std::variant<unsigned long long,float,double,unsigned,int,ID3D11Resource*,ID3D12Resource*,void*>;
    std::map<std::string,Value> values;
#define PARAM(T) void Set(const char* k,T v) override { values.insert_or_assign(k,v); } \
    NVSDK_NGX_Result Get(const char* k,T* v) const override { auto i=values.find(k); if(i==values.end()) return NVSDK_NGX_Result_Fail; auto p=std::get_if<T>(&i->second); if(!p) return NVSDK_NGX_Result_Fail; *v=*p; return NVSDK_NGX_Result_Success; }
    PARAM(unsigned long long) PARAM(float) PARAM(double) PARAM(unsigned) PARAM(int)
    PARAM(ID3D11Resource*) PARAM(ID3D12Resource*) PARAM(void*)
#undef PARAM
    void Reset() override { values.clear(); }
};
static unsigned creates=0,evaluates=0,releases=0,destroys=0,directCreates=0,allocations=0;
static std::map<const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*> handles;
static bool failCreate=false,failEval=false,failRelease=false,failDestroy=false;
static std::string order;
static Params* latest=nullptr;
static NVSDK_NGX_Result allocate(NVSDK_NGX_Parameter** p) { ++allocations; *p=latest=new Params; order+='P'; return NVSDK_NGX_Result_Success; }
static NVSDK_NGX_Result destroy(NVSDK_NGX_Parameter* p) { if(failDestroy) return NVSDK_NGX_Result_Fail; for(const auto& h:handles) check(h.second!=p); delete static_cast<Params*>(p); ++destroys; order+='D'; return NVSDK_NGX_Result_Success; }
static NVSDK_NGX_Result create(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature f,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h) {
    ++creates; order+='C'; check(static_cast<unsigned>(f)==18); unsigned w=0; float skin=0;
    check(p->Get("DLSSNR.Width",&w)==NVSDK_NGX_Result_Success && w==1920);
    check(p->Get("DLSSNR.SkinStructureStrength",&skin)==NVSDK_NGX_Result_Success && skin==-1);
    if(failCreate) return NVSDK_NGX_Result_Fail;
    *h=new NVSDK_NGX_Handle{}; handles[*h]=p; return NVSDK_NGX_Result_Success;
}
static NVSDK_NGX_Result directCreate(ID3D12GraphicsCommandList* c,NVSDK_NGX_Feature f,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h) {
    ++directCreates; const bool saved=failCreate; failCreate=false; auto r=create(c,f,p,h); failCreate=saved; return r;
}
static NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter* p,PFN_NVSDK_NGX_ProgressCallback) {
    check(handles.contains(h) && handles.at(h)==p);
    ++evaluates; order+='E'; unsigned reset=0; ID3D12Resource* color=nullptr;
    check(p->Get("DLSSNR.Reset",&reset)==NVSDK_NGX_Result_Success);
    check(p->Get("DLSSNR.Color",&color)==NVSDK_NGX_Result_Success && color);
    return failEval?NVSDK_NGX_Result_Fail:NVSDK_NGX_Result_Success;
}
static NVSDK_NGX_Result release(NVSDK_NGX_Handle* h) { if(failRelease) return NVSDK_NGX_Result_Fail; check(handles.erase(h)==1); delete h; ++releases; order+='R'; return NVSDK_NGX_Result_Success; }
static Settings settings() { return {{1},true,"C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll",{1},{0,0,1,1,0,-1,true}}; }
static JobContract job(uint64_t serial=1,uint64_t history=1) {
    return {{{1,1,serial},history,1,serial},{{1,1,serial},serial+1,serial,1,1,1,SourceValidity::Valid,
        {{1920,1080},{0,0,1920,1080},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},settings()},serial+2};
}
static Inputs inputs() { return {reinterpret_cast<ID3D12Resource*>(1),reinterpret_cast<ID3D12Resource*>(2),
    reinterpret_cast<ID3D12Resource*>(3),reinterpret_cast<ID3D12Resource*>(4),{0,0,1920,1080},{0,0,1920,1080},false,false,1,1,true}; }
static Dispatch api(bool direct=false) { return {allocate,destroy,direct?directCreate:create,evaluate,release}; }
static Recording recording(uint64_t epoch,const std::shared_ptr<Completion>& status) {
    return {reinterpret_cast<ID3D12GraphicsCommandList*>(1),epoch,std::make_shared<int>(1),[status]{return *status;}};
}
static void happy() {
    auto module=Module::controlled(api(),api(true),std::make_shared<int>(1),Route::DriverFallback);
    Pass pass(module); auto c=std::make_shared<Completion>(Completion::Recorded);
    check(pass.prepare(settings(),{1920,1080},1,recording(1,c))==NVSDK_NGX_Result_Success);
    check(!pass.ready(1) && !pass.ready(200)); *c=Completion::Complete;
    check(!pass.ready(1) && pass.ready(2));
    auto e=std::make_shared<Completion>(Completion::Recorded); auto j=job();
    check(pass.run(j,inputs(),recording(2,e))==NVSDK_NGX_Result_Success);
    check(!pass.completed(j.key)); *e=Completion::Complete;
    auto result=pass.completed(j.key); check(result && result->evaluated && result->workerComplete);
    pass.expire(j.key); check(pass.completed(j.key)->publication==PublicationState::Expired);
    check(!pass.publishable(j.key) && pass.release());
    check(order.find("PCE")!=std::string::npos && order.ends_with("RD"));
    puts("happy: parameters/create/later-completion/evaluate/expire/release PASS; CPU only");
}
static void refused() {
    auto module=Module::controlled(api(),api(true),std::make_shared<int>(1),Route::DriverFallback);
    Pass pass(module); auto c=std::make_shared<Completion>(Completion::Recorded);
    const unsigned before=evaluates,freed=releases;
    check(pass.prepare(settings(),{1920,1080},1,recording(1,c))==NVSDK_NGX_Result_Success);
    check(pass.run(job(),inputs(),recording(900,c))!=NVSDK_NGX_Result_Success);
    check(evaluates==before && releases==freed);
    *c=Completion::SignalFailed; check(!pass.ready(901));
    check(!pass.release() && releases==freed);
    *c=Completion::Complete; check(pass.release());
    puts("refused: unresolvedCreation/failedSignal prohibit evaluate/publish/release PASS; CPU only");
}
static void adversarial() {
    auto lease=std::make_shared<int>(1); std::weak_ptr<void> weak=lease;
    auto module=Module::controlled(api(),api(true),lease,Route::DriverFallback); lease.reset();
    auto c=std::make_shared<Completion>(Completion::Recorded); Pass pass(module);
    failCreate=true; const unsigned direct=directCreates;
    check(pass.prepare(settings(),{1920,1080},1,recording(1,c))==NVSDK_NGX_Result_Success);
    check(directCreates==direct+1); failCreate=false; *c=Completion::Complete; check(pass.ready(2));
    auto e=std::make_shared<Completion>(Completion::Recorded); failEval=true;
    check(pass.run(job(),inputs(),recording(2,e))!=NVSDK_NGX_Result_Success);
    check(!pass.publishable(job().key) && !pass.release()); *e=Completion::Complete;
    check(!pass.completed(job().key)->evaluated && pass.release()); failEval=false;
    Pass failed(Module::controlled(api(),{},std::make_shared<int>(1),Route::Driver));
    *c=Completion::Recorded; failCreate=true;
    check(failed.prepare(settings(),{1920,1080},1,recording(1,c))!=NVSDK_NGX_Result_Success);
    check(!failed.release()); *c=Completion::Discarded; check(failed.release()); failCreate=false;
    Pass a(module),b(module); *c=Completion::Recorded;
    check(a.prepare(settings(),{1920,1080},1,recording(1,c))==NVSDK_NGX_Result_Success); auto* pa=latest;
    check(b.prepare(settings(),{1920,1080},1,recording(1,c))==NVSDK_NGX_Result_Success); check(pa!=latest);
    *c=Completion::Complete; check(a.ready(2) && b.ready(2));
    *e=Completion::Recorded; check(a.run(job(),inputs(),recording(2,e))==NVSDK_NGX_Result_Success);
    a.cancel(job().key); check(!a.publishable(job().key));
    check(a.run(job(2),inputs(),recording(3,e))!=NVSDK_NGX_Result_Success);
    *e=Completion::Complete; check(!a.publishable(job().key));
    check(a.completed(job().key)->publication==PublicationState::Cancelled);
    check(a.release() && b.ready(2) && b.release()); check(!weak.expired());
    Pass history(module); *c=Completion::Recorded;
    check(history.prepare(settings(),{1920,1080},1,recording(1,c))==NVSDK_NGX_Result_Success);
    *c=Completion::Complete; *e=Completion::Recorded;
    check(history.run(job(),inputs(),recording(2,e))==NVSDK_NGX_Result_Success);
    unsigned reset=0; check(latest->Get("DLSSNR.Reset",&reset)==NVSDK_NGX_Result_Success && reset==1);
    *e=Completion::Complete;
    auto e2=std::make_shared<Completion>(Completion::Recorded);
    check(history.run(job(2),inputs(),recording(3,e2))==NVSDK_NGX_Result_Success);
    check(latest->Get("DLSSNR.Reset",&reset)==NVSDK_NGX_Result_Success && reset==0);
    *e2=Completion::Complete; auto e3=std::make_shared<Completion>(Completion::Recorded);
    check(history.run(job(3,2),inputs(),recording(4,e3))==NVSDK_NGX_Result_Success);
    check(latest->Get("DLSSNR.Reset",&reset)==NVSDK_NGX_Result_Success && reset==1);
    check(history.run(job(4,1),inputs(),recording(5,e))!=NVSDK_NGX_Result_Success);
    check(!history.release()); *e3=Completion::Complete; check(history.release());
    Pass directPass(Module::controlled(api(),api(true),std::make_shared<int>(1),Route::Direct));
    *c=Completion::Recorded;
    check(directPass.prepare(settings(),{1920,1080},1,recording(1,c))==NVSDK_NGX_Result_Success);
    *c=Completion::Discarded; check(!directPass.ready(2) && directPass.release());
    Submission real; check(real.state()==Completion::Recorded && real.discard());
    check(!real.submitted(nullptr) && real.state()==Completion::Discarded);
    Pass signaling(module); *c=Completion::Recorded;
    check(signaling.prepare(settings(),{1920,1080},1,recording(1,c))==NVSDK_NGX_Result_Success);
    *c=Completion::Complete; auto s=std::make_shared<Completion>(Completion::Recorded);
    check(signaling.run(job(),inputs(),recording(2,s))==NVSDK_NGX_Result_Success);
    *s=Completion::SignalFailed; check(!signaling.publishable(job().key));
    *s=Completion::Complete; check(!signaling.publishable(job().key)); // failed signal cannot resurrect output
    failRelease=true; const auto freed=releases,deleted=destroys;
    check(!signaling.release() && releases==freed && destroys==deleted);
    failRelease=false; failDestroy=true;
    check(!signaling.release() && releases==freed+1 && destroys==deleted);
    failDestroy=false; check(signaling.release() && releases==freed+1 && destroys==deleted+1);
    puts("adversarial: actual Pass fallback/direct/failure/history/cancel/independent maps PASS");
}
static void metadataTests() {
    std::vector<unsigned char> file(0x1400);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(file.data()); dos->e_magic=IMAGE_DOS_SIGNATURE; dos->e_lfanew=0x80;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data()+0x80);
    nt->Signature=IMAGE_NT_SIGNATURE; nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER64); nt->FileHeader.NumberOfSections=1;
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC; nt->OptionalHeader.SizeOfHeaders=0x400;
    nt->OptionalHeader.NumberOfRvaAndSizes=IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]={0x1000,0x80};
    auto* sec=reinterpret_cast<IMAGE_SECTION_HEADER*>(file.data()+0x80+sizeof(*nt));
    sec->VirtualAddress=0x1000; sec->PointerToRawData=0x400; sec->SizeOfRawData=0x1000;
    auto* exp=reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(file.data()+0x400);
    exp->NumberOfFunctions=5; exp->NumberOfNames=5; exp->AddressOfFunctions=0x1100;
    exp->AddressOfNames=0x1140; exp->AddressOfNameOrdinals=0x1180;
    const char* names[]={"NVSDK_NGX_D3D12_Init_Ext","NVSDK_NGX_D3D12_CreateFeature","NVSDK_NGX_D3D12_EvaluateFeature",
        "NVSDK_NGX_D3D12_ReleaseFeature","NVSDK_NGX_D3D12_Shutdown1"};
    for(unsigned i=0;i<5;++i) {
        auto fn=static_cast<DWORD>(0x1800+i); memcpy(file.data()+0x500+i*4,&fn,4);
        auto name=static_cast<DWORD>(0x1200+i*0x60); memcpy(file.data()+0x540+i*4,&name,4);
        auto ord=static_cast<WORD>(i); memcpy(file.data()+0x580+i*2,&ord,2);
        memcpy(file.data()+0x600+i*0x60,names[i],strlen(names[i])+1);
    }
    check(inspect_pe(file).accepted); exp->NumberOfNames=4;
    check(inspect_pe(file).reason=="missingExport"); exp->NumberOfNames=5;
    nt->FileHeader.Machine=IMAGE_FILE_MACHINE_I386; check(!inspect_pe(file).accepted);
    nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;
    DWORD forwarded=0x1010; memcpy(file.data()+0x500,&forwarded,4); check(!inspect_pe(file).accepted);
    dos->e_lfanew=-1; check(!inspect_pe(file).accepted);
    check(!inspect_pe(std::span<const unsigned char>(file.data(),10)).accepted);
    check(!inspect({"relative.dll",std::string(64,'0')}).accepted);
    // R5 moved ANSI/Unicode/ordinal import fixture, through production scanner.
    for(DWORD shift:{0u,0x300u}) {
        std::vector<unsigned char> image(0x4000);
        memcpy(image.data(),file.data(),0x400);
        auto* d=reinterpret_cast<IMAGE_DOS_HEADER*>(image.data()); d->e_lfanew=0x80;
        auto* n=reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data()+0x80);
        auto& dir=n->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        dir={0x400+shift,2*sizeof(IMAGE_IMPORT_DESCRIPTOR)};
        auto* desc=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.data()+dir.VirtualAddress);
        desc->Name=0x600+shift; desc->OriginalFirstThunk=0x800+shift; desc->FirstThunk=0xa00+shift;
        auto* imports=reinterpret_cast<IMAGE_THUNK_DATA64*>(image.data()+desc->OriginalFirstThunk);
        imports[0].u1.AddressOfData=0xc00+shift; imports[1].u1.AddressOfData=0xc40+shift;
        imports[2].u1.Ordinal=IMAGE_ORDINAL_FLAG64|123;
        memcpy(image.data()+0xc02+shift,"GetModuleFileNameW",18); memcpy(image.data()+0xc42+shift,"GetModuleFileNameA",18);
        std::vector<ImportSlot> slots; check(find_path_imports(image,slots) && slots.size()==2);
        check(slots[0].wide && !slots[1].wide);
        imports[1].u1.AddressOfData=image.size()-1; check(!find_path_imports(image,slots) && slots.empty());
        imports[1].u1.AddressOfData=0xc40+shift; dir.Size=sizeof(IMAGE_IMPORT_DESCRIPTOR);
        check(!find_path_imports(image,slots) && slots.empty());
    }
    puts("metadata: missingExport/corruptPE/forwardedExport/noncanonical/movedImports refusal PASS");
}
static void ownedModuleRetry() {
    auto lease=std::make_shared<int>(18);
    std::weak_ptr<int> retained=lease;
    bool allowed=false;
    unsigned attempts=0;
    const Dispatch dispatch{allocate,destroy,create,evaluate,release};
    auto owner=Module::controlled(dispatch,{},lease,Route::Driver,[&] { ++attempts;return allowed; });
    auto independent=Module::controlled(dispatch,{},std::make_shared<int>(19),Route::Driver);
    auto* identity=owner.get();
    lease.reset();
    {
        Pass live(owner);
        check(!owner->close() && attempts==0);
    }
    check(!owner->close() && owner.get()==identity && !retained.expired());
    check(!owner->close() && owner.get()==identity && attempts==2);
    check(independent->close() && !retained.expired());
    allowed=true;
    check(owner->close() && owner.get()==identity && attempts==3);
    check(owner->close() && attempts==3);
    owner.reset();
    check(retained.expired());
    puts("PASS actual owned Module refusal/same-owner/retry/idempotence/independent owner");
}
static void vendorTrace() {
    nr::lifecycle::VendorTrace trace, independent;
    check(trace.snapshot().owner==0 && trace.snapshot().calls[0].count==0);
    check(trace.bind(11,22,33,44));
    check(!trace.bind(11,22,34,44));
    check(independent.bind(51,52,53,54));
    const nr::lifecycle::VendorBinding binding{61,62,63};
    const auto refused=trace.call(nr::lifecycle::VendorStage::CoreInit,binding,[&] {
        const auto pending=trace.snapshot();
        check(pending.calls[0].count==1 && pending.calls[0].first.enter && !pending.calls[0].first.returned);
        return nr::lifecycle::VendorResult{0xBAD00007u,0,0};
    });
    check(refused.result==0xBAD00007u);
    trace.call(nr::lifecycle::VendorStage::CoreInit,binding,[] {return nr::lifecycle::VendorResult{1,0,0};});
    trace.call(nr::lifecycle::VendorStage::NrShutdown,binding,[] {return nr::lifecycle::VendorResult{1,0,0};});
    trace.call(nr::lifecycle::VendorStage::CapabilityDestroy,binding,[] {return nr::lifecycle::VendorResult{1,0,0};});
    trace.call(nr::lifecycle::VendorStage::NrLibraryRelease,binding,[] {return nr::lifecycle::VendorResult{1,0,0};});
    trace.call(nr::lifecycle::VendorStage::CoreShutdown,binding,[] {return nr::lifecycle::VendorResult{0xBAD00007u,0,0};});
    trace.call(nr::lifecycle::VendorStage::CoreShutdown,binding,[] {return nr::lifecycle::VendorResult{1,2,0};});
    const auto d=trace.snapshot();
    check(d.coherent && d.owner==11 && d.instance==22 && d.generation==33 && d.device==44);
    check(d.calls[0].first.result==0xBAD00007u && d.calls[0].latest.result==1 && d.calls[0].count==2);
    check(d.calls[2].first.returned<d.calls[3].first.enter && d.calls[3].first.returned<d.calls[4].first.enter);
    check(d.calls[4].first.returned<d.calls[5].first.enter);
    check(d.calls[5].first.result==0xBAD00007u && d.calls[5].first.remaining==0 && d.calls[5].latest.remaining==2);
    check(d.calls[5].count==2 && d.calls[5].first.module==61 && d.calls[5].first.procedure==62 && d.calls[5].first.thread==63);
    check(!d.nestedCoreObserved && independent.snapshot().calls[5].count==0 && independent.snapshot().device==54);
    check(nr::lifecycle::vendor_return_kind(nr::lifecycle::VendorStage::NrLibraryRelease)==nr::lifecycle::VendorReturnKind::Win32Bool);
    check(nr::lifecycle::vendor_return_kind(nr::lifecycle::VendorStage::CapabilityDestroy)==nr::lifecycle::VendorReturnKind::SdkResult);
    independent.call(nr::lifecycle::VendorStage::CoreLibraryRelease,binding,[] {return nr::lifecycle::VendorResult{0,0,5};});
    check(independent.snapshot().calls[6].first.returned && independent.snapshot().calls[6].first.result==0 && independent.snapshot().calls[6].first.error==5);
    try {independent.call(nr::lifecycle::VendorStage::CoreShutdown,binding,[]()->nr::lifecycle::VendorResult {throw std::runtime_error("controlled boundary exception");});}
    catch(const std::runtime_error&) {}
    independent.call(nr::lifecycle::VendorStage::CoreShutdown,binding,[] {return nr::lifecycle::VendorResult{1,0,0};});
    check(independent.snapshot().calls[5].first.enter && !independent.snapshot().calls[5].first.returned && independent.snapshot().calls[5].latest.returned);
    nr::lifecycle::ModelClose proof;
    proof.settlement=proof.feature=proof.ofa=proof.module=proof.core=nr::lifecycle::CloseResult::Complete;
    proof.coreCalled=true;proof.coreResult=0xBAD00007u;proof.coreRemaining=0;proof.vendor=d;
    check(nr::lifecycle::close_model(proof)==nr::lifecycle::CloseBlock::CoreShutdown);
    puts("PASS bounded production VendorTrace first/latest/ordering/owner/default-unknown; no vendor execution");
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="happy") happy();
        else if(argc==2 && std::string(argv[1])=="refused") refused();
        else if(argc==2 && std::string(argv[1])=="metadata") {
            auto r=inspect({settings().runtimePath,"e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a"});
            check(r.accepted && !r.loaded); puts("owner canonical path/hash/PE64/direct exports PASS; not loaded");
        } else if(argc==2 && std::string(argv[1])=="close") ownedModuleRetry();
        else if(argc==2 && std::string(argv[1])=="trace") vendorTrace();
        else if(argc==1) { happy(); refused(); adversarial(); metadataTests(); ownedModuleRetry(); vendorTrace(); }
        else return 2;
        check(handles.empty() && allocations==destroys);
        printf("PASS runtime_contract: %u checks\n",checks); return 0;
    } catch(const std::exception& e) { fprintf(stderr,"FAIL: %s (%u checks)\n",e.what(),checks); return 1; }
}
