// SPDX-License-Identifier: GPL-3.0-only
#include "obs_native_output.h"
#include <cstdio>
#include <stdexcept>
#include <string_view>

void production_closure_policy()
{
    using namespace nr::lifecycle;
    unsigned checks=0;
    const auto expect=[&](bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);};
    struct ModelActions {
        ModelClose& proof;
        std::array<bool,5> allowed{true,true,true,true,true};
        std::string order;
        uint32_t remaining=0,result=1;
        bool step(size_t i) {
            order+=static_cast<char>('0'+i);
            if(i==4){proof.coreCalled=true;proof.coreResult=result;proof.coreRemaining=remaining;}
            return allowed[i];
        }
    };
    const std::array<CloseBlock,5> stages{CloseBlock::ModelSettlement,CloseBlock::FeatureRelease,
        CloseBlock::OfaRelease,CloseBlock::ModuleRelease,CloseBlock::CoreShutdown};
    for(size_t blocked=0;blocked<stages.size();++blocked) {
        ModelClose proof;
        ModelActions actions{proof};
        actions.allowed[blocked]=false;
        expect(!close_model_steps(proof,actions) && close_model(proof)==stages[blocked],"model refusal lost exact stage");
        expect(actions.order.size()==blocked+1,"model refusal executed downstream release");
        actions.allowed[blocked]=true;
        expect(close_model_steps(proof,actions) && actions.order.ends_with("01234"),"same model owner did not retry in order");
    }
    ModelClose core;
    ModelActions coreActions{core};coreActions.remaining=1;
    expect(!close_model_steps(core,coreActions),"CORE remaining ownership accepted");
    coreActions.remaining=0;coreActions.result=0xbad;
    expect(!close_model_steps(core,coreActions),"CORE failed result accepted");
    coreActions.result=1;
    expect(close_model_steps(core,coreActions),"CORE retry did not collect");
    struct GenerationActions {
        std::string order;
        bool closed=true,bridge=true,generation=true;
        ModelClose model;
        GenerationActions() {
            model.settlement=model.feature=model.ofa=model.module=model.core=CloseResult::Complete;
        }
        void release_inputs(){order+='R';}
        void request_model(){order+='M';}
        bool model_closed(){order+='C';return closed;}
        ModelClose model_receipt() const {return model;}
        void retry_model(){order+='S';}
        bool close_bridge(){order+='B';return bridge;}
        bool complete_generation(){order+='G';return generation;}
    };
    const auto ready=[] {
        CloseGeneration g;
        g.id=1;g.quiescent=true;g.hasModel=true;g.graphics=g.graphicsDone=4;
        g.slots[0]={9,1,1,2,2,3,3,true,true,true,false};
        return g;
    };
    const std::array<uint64_t CloseSlot::*,3> fenceDone{&CloseSlot::producerDone,&CloseSlot::workerDone,&CloseSlot::consumerDone};
    const std::array<CloseBlock,3> fenceBlocks{CloseBlock::Producer,CloseBlock::Worker,CloseBlock::Consumer};
    for(size_t i=0;i<fenceDone.size();++i)for(const auto value:{uint64_t{0},UINT64_MAX}) {
        auto g=ready();GenerationActions actions;g.slots[0].*fenceDone[i]=value;
        expect(close_generation_steps(g,false,actions)==fenceBlocks[i] && actions.order.empty(),"pending/unknown fence released resources");
    }
    for(unsigned condition=0;condition<7;++condition) {
        auto g=ready();GenerationActions actions;CloseBlock wanted;
        switch(condition) {
        case 0:g.slots[0].ready=false;wanted=CloseBlock::Unready;break;
        case 1:g.slots[0].returned=false;wanted=CloseBlock::Unreturned;break;
        case 2:g.slots[0].pin=true;wanted=CloseBlock::OpenPin;break;
        case 3:g.graphicsDone=0;wanted=CloseBlock::Graphics;break;
        case 4:g.graphicsDone=UINT64_MAX;wanted=CloseBlock::Graphics;break;
        case 5:g.cutoffLeases=1;wanted=CloseBlock::CutoffLease;break;
        default:g.quiescent=false;wanted=CloseBlock::Consumer;break;
        }
        expect(close_generation_steps(g,false,actions)==wanted && actions.order.empty(),"unresolved native receipt released inputs");
    }
    auto g=ready();GenerationActions actions;
    expect(close_generation_steps(g,true,actions)==CloseBlock::Consumer && actions.order.empty(),"live dispatch ownership released");
    actions.closed=false;
    expect(close_generation_steps(g,false,actions)==CloseBlock::ModelSettlement && actions.order=="RMCS","model refusal did not request cooperative retry");
    actions.closed=true;actions.bridge=false;actions.order.clear();
    expect(close_generation_steps(g,false,actions)==CloseBlock::Bridge && actions.order=="RMCB","bridge refusal collected generation");
    actions.bridge=true;actions.generation=false;actions.order.clear();
    expect(close_generation_steps(g,false,actions)==CloseBlock::Generation && actions.order=="RMCBG","generation refusal was discarded");
    actions.generation=true;actions.order.clear();
    expect(close_generation_steps(g,false,actions)==CloseBlock::None && actions.order=="RMCBG" && !g.slots[0].occupied,"ordered physical-policy collection failed");
    auto stale=ready();stale.closed=true;stale.bridge=stale.generation=CloseResult::Complete;
    GenerationActions refused;refused.bridge=false;
    expect(close_generation_steps(stale,false,refused)==CloseBlock::Bridge,
        "stale successful diagnostic bypassed current close action refusal");
    stale=ready();refused.bridge=true;refused.model.coreRemaining=1;
    expect(close_generation_steps(stale,false,refused)==CloseBlock::CoreShutdown,
        "worker closed flag bypassed actual remaining CORE receipt");
    struct PipelineActions {
        bool allowed=false;
        unsigned closedCalls=0,releases=0;
        bool try_close(){++closedCalls;return allowed;}
        void release(){++releases;}
    } downstream;
    CloseSnapshot finalProof;finalProof.stop=true;
    expect(!close_pipeline_steps(finalProof,downstream) && finalProof.block==CloseBlock::Pipeline &&
        downstream.releases==0,"pipeline refusal released owners");
    downstream.allowed=true;
    expect(close_pipeline_steps(finalProof,downstream) && close_complete(finalProof) &&
        downstream.releases==1,"pipeline retry failed final closure");
    expect(close_pipeline_steps(finalProof,downstream) && downstream.closedCalls==2 &&
        downstream.releases==1,"final closure was not idempotent");
    CloseSnapshot entry;entry.stop=true;entry.owner=5;entry.caller=6;
    unsigned observed=0;
    expect(close_entry_steps(entry,false,0,0,[&]{++observed;})==CloseBlock::WrongOwner && !observed,"wrong graphics owner executed action");
    entry.caller=5;
    expect(close_entry_steps(entry,true,7,7,[&]{++observed;})==CloseBlock::MissingTick && !observed,"missing tick executed action");
    expect(close_entry_steps(entry,true,8,7,[&]{++observed;})==CloseBlock::None && observed==1,"later tick refused action");
    ReplacementBaseline replacement;replacement.retire(41);
    for(unsigned warm=0;warm<4;++warm)expect(replacement.accepts(42) && replacement.original==41,"warmup overwrote retired original identity");
    expect(!replacement.accepts(41) && !replacement.accepts(0),"replacement identity stale/empty accepted");
    expect(!settings_applied(3,2,3) && !settings_applied(3,3,2),"stale settings accepted");
    expect(settings_applied(3,3,3),"applied settings never accepted");
    CloseSnapshotStore archive;
    {
        nr::obs_adapter::NativeOutput source(97,nr::model::Config{},{},{},{},true);
        source.request_stop();expect(source.try_close(),"never-prepared source closure failed");
        archive.record(source.close_diagnostics(true));
    }
    const auto surviving=archive.read();
    expect(surviving.sourceDetached && surviving.instance==97 && close_complete(surviving),"production module storage/read seam did not survive destruction");
    expect(!close_complete(archive.read(1)) && archive.read(1).retiringCount==1,
        "latest closed replacement hid an older retained output");
    expect(close_complete(archive.read(0)),"all-owner closure remained refused after collection");
    auto aborted=surviving;aborted.closed=false;aborted.workerExited=true;archive.record(aborted);
    expect(!close_complete(archive.read()),"abort was classified normal");
    std::printf("PASS production shared closure caller receipt/action policies checks=%u CPU_only=true physical=false registered_OBS_procedure=false\n",checks);
}

int main(int argc,char** argv)
{
    try {
        if(argc==2 && std::string_view(argv[1])=="--production-closure") {production_closure_policy();return 0;}
        if(argc==2 && std::string_view(argv[1])=="--closure") {
            nr::lifecycle::CloseSnapshot primitive;
            {
                nr::obs_adapter::NativeOutput source(73,nr::model::Config{},{},{},{},true);
                if(source.try_close() || source.close_diagnostics().block!=nr::lifecycle::CloseBlock::NotStopping)
                    throw std::runtime_error("accepting source claimed normal closure");
                source.request_stop();
                if(!source.try_close())throw std::runtime_error("actual never-prepared source close refused");
                primitive=source.close_diagnostics(true);
                if(!nr::lifecycle::close_complete(primitive) || primitive.workerExited)
                    throw std::runtime_error("actual never-prepared close invented worker exit");
                if(!source.try_close() || source.close_diagnostics().sequence!=primitive.sequence)
                    throw std::runtime_error("successful close was not idempotent");
            }
            if(!primitive.sourceDetached || primitive.instance!=73 || !nr::lifecycle::close_complete(primitive))
                throw std::runtime_error("primitive diagnostic did not survive actual output destruction");
            auto aborted=primitive;aborted.closed=false;aborted.workerExited=true;
            if(nr::lifecycle::close_complete(aborted))
                throw std::runtime_error("unresolved OS-abort classification became normal closure");
            std::puts("PASS 6 actual close/idempotence/destruction/primitive-survival/abort controls; CPU only");
            return 0;
        }
        if(argc!=1)throw std::runtime_error("unknown native lifetime invocation");
        nr::obs_adapter::NativeOutput output(71,nr::model::Config{},{},{},{},true);
        if(output.worker_exit().valid() || output.worker_exit_signal() || output.context() || output.transport())
            throw std::runtime_error("control-thread construction acquired graphics or worker ownership");
        if(output.try_close() || output.closed())
            throw std::runtime_error("unprepared accepting output closed without a stop request");
        const auto epochs=output.epochs();
        if(epochs.instanceId!=71 || epochs.contentEpoch!=1 || epochs.historyEpoch!=1 || epochs.resourceGeneration!=1)
            throw std::runtime_error("deferred pipeline changed initial immutable epochs");
        output.request_stop();
        if(!output.try_close() || !output.closed() || output.worker_exit().valid() || output.worker_exit_signal())
            throw std::runtime_error("never-prepared output did not close without inventing a worker exit");
        std::puts("PASS 4 deferred graphics-owner and never-prepared close controls; CPU only");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL %s\n",error.what());
        return 1;
    }
}
