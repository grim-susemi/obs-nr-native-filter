// SPDX-License-Identifier: GPL-3.0-only
// Branches of the NGX Shutdown1 skip decision, driven by synthetic memory pages. No GPU, no OBS, no NGX.
#include "nr_model.h"
#include <cstdio>
#include <stdexcept>
#include <windows.h>
namespace {
unsigned checks=0;
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
unsigned char* page(){
    auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,2*4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    expect(memory!=nullptr,"test page allocation");
    return memory;
}
}
int main() try {
    auto* own=page();
    auto* other=page();
    const auto owner=reinterpret_cast<HMODULE>(own);
    own[0]=0x48;own[1]=0x89;
    own[32]=0xE9;
    own[48]=0xFF;own[49]=0x25;
    own[64]=0xFF;own[65]=0x15;
    own[4096+16]=0x48;own[4096+17]=0x89;
    own[4096+32]=0xE9;
    expect(!nr::model::entry_interposed(nullptr,own),"null owner is not interposed");
    expect(!nr::model::entry_interposed(owner,nullptr),"null entry is not interposed");
    expect(!nr::model::entry_interposed(owner,own),"own image with a normal prologue is not interposed");
    expect(nr::model::entry_interposed(owner,own+32),"E9 detour jump is interposed");
    expect(nr::model::entry_interposed(owner,own+48),"FF 25 indirect jump is interposed");
    expect(!nr::model::entry_interposed(owner,own+64),"FF 15 indirect call is not a detour jump");
    expect(!nr::model::entry_interposed(owner,own+4096+16),"normal prologue on a later page of the own image is not interposed");
    expect(nr::model::entry_interposed(owner,own+4096+32),"E9 on a later page of the own image is interposed");
    expect(nr::model::entry_interposed(owner,other),"entry served by another image is interposed");
    VirtualFree(own,0,MEM_RELEASE);
    VirtualFree(other,0,MEM_RELEASE);
    std::printf("interposed_entry: %u checks passed\n",checks);
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return 1;
}
