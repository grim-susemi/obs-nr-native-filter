// SPDX-License-Identifier: GPL-3.0-only
// UG03 P03: a host keeps module-resident QaSignal owners (vtable, destructor and
// future state manager live in the exporting module) after a successful Close.
// The exporting module must stay mapped until those final references are gone,
// whatever the loader's reference count does. Uses the existing production
// signal probe module (nr-qa-signal-contract), not a second fixture.
#include "nr_model.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <stdexcept>
using Signals=std::array<std::shared_ptr<nr::model::QaSignal>,3>;
namespace {
unsigned checks=0;
Signals* keptOnFailure=nullptr;
void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class T>T function(HMODULE module,const char* name) {
    const auto address=GetProcAddress(module,name);expect(address!=nullptr,"module probe export");
    return reinterpret_cast<T>(address);
}
bool mapped(HMODULE module) {
    HMODULE found=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(module),&found) && found==module;
}
}
int wmain(int argc,wchar_t** argv)try {
    expect(argc==2,"exact module probe path");
    const auto module=LoadLibraryW(argv[1]);expect(module!=nullptr,"module probe load");
    const auto create=function<void*(*)(Signals*)>(module,"nr_qa_signal_probe_create");
    const auto release=function<void(*)(void*)>(module,"nr_qa_signal_probe_release");
    const auto retire=function<bool(*)(void*)>(module,"nr_qa_signal_probe_retire");
    const auto destroy=function<void(*)(void*)>(module,"nr_qa_signal_probe_destroy");
    const auto live=function<uint64_t(*)()>(module,"nr_qa_signal_probe_live");
    Signals signals;auto* probe=create(&signals);expect(probe!=nullptr,"actual production signal probe");
    expect(signals[0]->wait_for(5000),"exact entered subscription");
    release(probe);expect(signals[1]->wait_for(5000),"real production dispatch returned");
    expect(retire(probe),"real production worker exit and pipeline retirement (successful close)");
    destroy(probe);
    expect(live()==3,"host holds the final module-resident signal owners");
    // The host's own loader reference ends while it still holds the exported owners.
    expect(FreeLibrary(module)!=FALSE,"host loader reference release");
    if(!mapped(module)) {
        // Releasing these now would run an unmapped destructor: keep them, report RED.
        keptOnFailure=new Signals(std::move(signals));
        throw std::runtime_error("exporting module unmapped while host holds its signal owners");
    }
    expect(live()==3 && signals[2]->ready(),"retained module code serves the exported owners");
    for(auto& signal:signals)signal.reset();
    expect(live()==0,"final host release ran the module destructors");
    expect(mapped(module),"exporting module stays mapped after final release");
    std::printf("PASS qa_export_pin checks=%u module_mapped_through_final_release=true CPU_only=true\n",checks);
    return 0;
}catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
