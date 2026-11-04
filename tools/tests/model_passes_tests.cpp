// SPDX-License-Identifier: GPL-3.0-only
#include "nr_model_passes.h"
#include "nr_model_layout.h"
#include <cstdio>
#include <stdexcept>
using namespace nr;
using namespace nr::model;
namespace {
unsigned checks=0;
void expect(bool ok,const char* why) {++checks;if(!ok)throw std::runtime_error(why);}
struct FakePass {
    const uint64_t creationEpoch;
    bool creationComplete=false,evaluationComplete=true;
    unsigned& releases;
    FakePass(uint64_t epoch,unsigned& count):creationEpoch(epoch),releases(count){}
    bool ready(uint64_t epoch) const {return creationComplete && epoch>creationEpoch;}
    bool release() {if(!creationComplete || !evaluationComplete)return false;++releases;return true;}
};
using Passes=std::array<std::unique_ptr<FakePass>,3>;
void creation_gate() {
    // Given three independent requested histories and no created object.
    PassChain chain;chain.reset(3);Passes passes;unsigned releases=0;
    for(uint32_t i=0;i<3;++i) {
        const auto before=chain.gate(passes,i+1);
        expect(before.create==i && before.ready==i && !before.primed(),"create only first missing history");
        passes[i]=std::make_unique<FakePass>(i+1,releases);
        // When an API return has not yet physically completed.
        const auto pending=chain.gate(passes,i+2);
        // Then neither inference nor another creation may proceed.
        expect(!pending.primed() && !pending.create && pending.ready==i,"API success admitted before completion");
        passes[i]->creationComplete=true;
        const auto sameEpoch=chain.gate(passes,i+1);
        expect(!sameEpoch.primed() && !sameEpoch.create,"two creations in same job epoch");
    }
    const auto ready=chain.gate(passes,4);
    expect(ready.primed() && ready.ready==3 && !ready.create,"all requested histories gate inference");
    expect(chain.status()==PassStatus{3,0,0,0},"readiness is not physical evaluated output");
    expect(release_passes(passes) && releases==3,"all independent histories retired");
}
void failures() {
    for(uint32_t failed:{2u,3u}) for(bool composed:{false,true}) {
        // Given a settled three-history chain that previously published three passes.
        PassChain chain;chain.reset(3);Passes passes;unsigned releases=0;
        for(uint32_t i=0;i<3;++i) {passes[i]=std::make_unique<FakePass>(1,releases);passes[i]->creationComplete=true;}
        expect(chain.settle({3,0,0,true,true}),"initial physical result");
        const auto before=chain.status();passes[failed-1]->evaluationComplete=false;
        const uint32_t code=composed?0xe019u:0xbad00001u;
        // When the failed evaluation or safe clamp has only been recorded, not physically settled.
        const bool early=chain.settle({failed-1,failed,code,false,composed});
        // Then no failure reduction or owner release is invented.
        expect(!early && chain.status()==before,"recorded result became physical status");
        expect(!release_passes(passes,failed-1) && passes[failed-1],"uncompleted history owner released");
        passes[failed-1]->evaluationComplete=true;
        // Given the original completion/discard receipt. When settling that exact result.
        expect(chain.settle({failed-1,failed,code,true,composed}),"exact result settlement");
        // Then the next count is permanently reduced, including safe clamp fallback.
        expect(chain.status()==PassStatus{3,failed-1,failed,code},"failed pass/status convention");
        expect(release_passes(passes,chain.limit()),"failed and following histories did not retire");
        for(uint64_t epoch=4;epoch<12;++epoch) {
            const auto gate=chain.gate(passes,epoch);
            expect(gate.primed() && !gate.create && gate.ready==failed-1,"latched history retried");
        }
        expect(chain.settle({3,0,0,true,true}) && chain.status().effective==failed-1,"effective count increased after failure");
        const auto encoded=pack_pass_status(chain.status());
        expect(unpack_pass_status(encoded)==PassStatus{3,failed-1,failed,code},"atomic snapshot lost full code/count");
        expect(release_passes(passes),"remaining histories retire");
        chain.reset(3);
        expect(chain.status()==PassStatus{3,0,0,0} && chain.gate(passes,20).create==0u,"regeneration did not clear failure");
    }
}
void creation_failure_and_instances() {
    // Given a first ready history and an incomplete failed second creation.
    PassChain a,b;a.reset(3);b.reset(1);Passes passes;unsigned releases=0;
    passes[0]=std::make_unique<FakePass>(1,releases);passes[0]->creationComplete=true;
    passes[1]=std::make_unique<FakePass>(2,releases);
    // When the second creation's completion is still unknown.
    expect(!a.settle({0,2,0xbad00002u,false,false}),"creation API failure invented completion");
    // Then no effective inference or reduction is published.
    expect(!a.gate(passes,3).primed() && a.status()==PassStatus{3,0,0,0},"unsettled creation exposed NR");
    passes[1]->creationComplete=true;
    expect(a.settle({0,2,0xbad00002u,true,false}) && release_passes(passes,a.limit()),"failed creation retirement");
    expect(a.gate(passes,4).primed() && !a.gate(passes,4).create,"failed creation retried");
    expect(a.settle({1,0,0,true,true}) && a.status()==PassStatus{3,1,2,0xbad00002u},"reduced first physical inference");
    expect(b.status()==PassStatus{1,0,0,0},"instances shared pass status");
    expect(a.settle({0,1,0xbad00003u,true,false}) && a.status().effective==1 && a.status().failedPass==2,
        "first-history retry cleared generation-local later failure");
    expect(release_passes(passes),"first history cleanup");
}
void layouts() {
    constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    for(bool reduced:{false,true}) for(bool multi:{false,true}) {
        // Given one native/work/multipass pool key.
        const Extent native{1281,721},work=reduced?Extent{641,361}:native;
        const ColorLayout layout{native,work,multi};const auto sizes=layout.extents();
        const auto initial=layout.initial_states();
        // When choosing allocations and the successful final state table.
        expect(sizes[0]==native && sizes[1]==native && sizes[2]==native && sizes[3]==work && sizes[4]==native,
            "native/work allocation split");
        // Then extra allocations are conditional and only the final answer is readable.
        expect(sizes[5]==(reduced?work:Extent{0,0}) && sizes[6]==(multi?work:Extent{0,0}) &&
            sizes[7]==(multi?work:Extent{0,0}),"unused work resources allocated");
        expect(initial[0]==write && initial[1]==write && initial[2]==write && initial[4]==write &&
            initial[3]==(reduced || multi?D3D12_RESOURCE_STATE_COMMON:write) &&
            initial[5]==D3D12_RESOURCE_STATE_COMMON && initial[6]==D3D12_RESOURCE_STATE_COMMON &&
            initial[7]==D3D12_RESOURCE_STATE_COMMON,"discarded new work scratch does not remain COMMON");
        for(uint32_t count:{1u,2u,3u}) {
            const auto states=final_color_states(count);
            expect(states[0]==read && states[1]==read && states[2]==read && states[4]==read && states[5]==read &&
                states[3]==(count%2?read:write) && states[6]==(count%2?write:read) && states[7]==write,
                "final A/B/C COMMON handoff uses wrong prior state");
        }
        expect(layout!=ColorLayout{native,work,!multi} &&
            layout!=ColorLayout{native,{640,360},multi} &&
            layout!=ColorLayout{{1280,720},work,multi},"pool mixed native/work/multipass keys");
    }
}
}
int main() try {
    creation_gate();failures();creation_failure_and_instances();layouts();
    std::printf("PASS model_contract checks=%u creation/readiness/latches/physical-status/release/instances/layout/states CPU_only=true\n",checks);
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s checks=%u\n",e.what(),checks);return 1;}
