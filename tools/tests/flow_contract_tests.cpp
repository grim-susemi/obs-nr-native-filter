// SPDX-License-Identifier: GPL-3.0-only
#include "nr_guides.h"
#include "nr_flow_scenario.h"
#include <sstream>
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace nr;
static unsigned checks=0;
static void require(bool b,const char* m) { ++checks; if(!b) throw std::runtime_error(m); }
int main(int argc,char**) {
 try {
  require(argc==1,"malformed invocation");
  std::ostringstream admission;
  require(nr::qa::run_flow_translation(nullptr,nullptr,{},false,admission)==2,"CPU-only guard must refuse before device/API access");
  require(!smallest_flow_grid({}),"empty capability accepted");
  const std::array<uint32_t,3> caps{4,1,2};
  require(smallest_flow_grid(caps)==1u,"smallest queried grid");
  const std::array<uint32_t,1> wrong{8};
  require(!smallest_flow_grid(wrong),"unknown SDK grid accepted");
  for(auto width: {1920u,2560u}) {
   const Extent size{width,width==1920?1080u:1440u};
   const Rect full{0,0,size.width,size.height};
   for(auto amount:{1,8,32}) for(auto sign:{-1,1}) for(auto axis:{0,1}) {
    const auto v=decode_flow(static_cast<int16_t>(axis==0?sign*amount*32:0),static_cast<int16_t>(axis==1?sign*amount*32:0));
    require(v.x==(axis==0?static_cast<float>(sign*amount):0.f) && v.y==(axis==1?static_cast<float>(sign*amount):0.f),"signed10.5 units/direction");
    require(flow_geometry_valid(size,full),"1080/1440 full geometry");
   }
   const Rect sub{16,24,size.width-32,size.height-48};
   require(flow_geometry_valid(size,sub),"active subrect");
   require(decode_flow(-1,1).x==-0.03125f && decode_flow(-1,1).y==0.03125f,"fractional signed units");
  }
  require(!flow_geometry_valid({1920,1080},{1919,0,2,1}),"out of range rectangle");
  const JobKey prev{{41,2,10},3,4,11},current{{41,2,11},3,4,12};
  require(flow_pair_problem(current,&prev,false)==FlowProblem::None,"current/reference chronology");
  require(flow_pair_problem(current,nullptr,true)==FlowProblem::None,"cold reset admission");
  require(flow_pair_problem(current,nullptr,false)==FlowProblem::MissingPrevious,"ordinary zero-flow rejected");
  require(flow_pair_problem(current,&prev,true)==FlowProblem::ResetPair,"reset cannot reuse old guides");
  const JobKey old{{41,2,10},2,4,11},future{{41,2,12},3,4,11};
  require(flow_pair_problem(current,&old,false)==FlowProblem::KeyMismatch,"old guide epoch rejected");
  require(flow_pair_problem(current,&future,false)==FlowProblem::KeyMismatch,"future reference rejected");
  require(!flow_cost_format_valid(DXGI_FORMAT_R32_FLOAT),"cost is not probability");
  require(flow_cost_format_valid(DXGI_FORMAT_R8_UINT),"queried cost format");
  const Rect active{16,24,1888,1032},other{0,0,1920,1080};
  require(flow_chronology_problem(current,&prev,&prev,active,&active,false)==FlowProblem::None,"cached chronology");
  require(flow_chronology_problem(current,&prev,&prev,other,&active,false)==FlowProblem::KeyMismatch,"active rectangle changed without reset");
  require(flow_chronology_problem(prev,nullptr,&prev,active,&active,true)==FlowProblem::KeyMismatch,"repeated reset key");
  const JobKey resetKey{{41,3,12},4,4,13};
  require(flow_chronology_problem(resetKey,nullptr,&current,active,&active,true)==FlowProblem::None,"cut reset advances epoch");
  require(flow_chronology_problem(current,&prev,nullptr,active,nullptr,false)==FlowProblem::MissingPrevious,"resume cannot infer temporal state");
  const auto init=flow_init_params({1920,1080},1);
  require(init.width==1920 && init.height==1080 && init.outGridSize==NV_OF_OUTPUT_VECTOR_GRID_SIZE_1,"full working-size SDK init");
  require(init.predDirection==NV_OF_PRED_DIRECTION_FORWARD && init.enableOutputCost==NV_OF_TRUE &&
      init.enableExternalHints==NV_OF_FALSE && init.mode==NV_OF_MODE_OPTICALFLOW,"SDK direction/cost/no fallback");
  const auto c=reinterpret_cast<NvOFGPUBufferHandle>(uintptr_t(1));
  const auto p=reinterpret_cast<NvOFGPUBufferHandle>(uintptr_t(2));
  std::array<NV_OF_FENCE_POINT,1> deps{{{nullptr,7}}};
  const auto packet=flow_execute_input(c,p,true,deps);
  require(packet.inputFrame==c && packet.referenceFrame==p,"production packet current/reference order");
  require(packet.disableTemporalHints==NV_OF_TRUE && packet.numFencePoints==1 && packet.fencePoint==deps.data(),"reset/dependency packet");
  require(packet.padding==0 && packet.padding2==0 && packet.padding3==0 && !packet.hPrivData && !packet.externalHints &&
      !packet.numRois && !packet.roiData,"reserved API fields zero");
  require(flow_execute_input(c,p,false,deps).disableTemporalHints==NV_OF_FALSE,"ordinary temporal hints");
  const std::array<uint32_t,4> hintCaps{8,4,1,2};
  require(smallest_hint_grid(hintCaps,1)==1u && smallest_hint_grid(hintCaps,4)==4u,"admitted hint grid/output ordering");
  require(!smallest_hint_grid({},1) && !smallest_hint_grid(wrong,16),"missing or too-small hint grid");
  const std::array<uint32_t,1> invalidHint{3};
  require(!smallest_hint_grid(invalidHint,1),"unknown hint grid");
  const auto bootstrap=flow_bootstrap_params({2560,1440},1),refine=flow_refinement_params({2560,1440},1,1);
  require(bootstrap.enableGlobalFlow==NV_OF_TRUE && bootstrap.enableExternalHints==NV_OF_FALSE,"bootstrap estimates native global, no external predictor");
  require(refine.enableExternalHints==NV_OF_TRUE && refine.enableGlobalFlow==NV_OF_FALSE && refine.hintGridSize==NV_OF_HINT_VECTOR_GRID_SIZE_1,"refinement admits exact hint grid, distinct pass");
  require(bootstrap.perfLevel==refine.perfLevel && bootstrap.outGridSize==refine.outGridSize && refine.width==2560 && refine.height==1440,"uniform full-resolution native estimator");
  const auto hint=reinterpret_cast<NvOFGPUBufferHandle>(uintptr_t(3));
  const auto refinePacket=flow_execute_input(c,p,true,deps,hint);
  require(refinePacket.externalHints==hint && refinePacket.disableTemporalHints==NV_OF_TRUE && refinePacket.fencePoint==deps.data(),"refinement uses this-pair GPU hint, never prior-call temporal field");
  std::printf("PASS flow_contract %u assertions; CPU only, no device/API initialization\n",checks); return 0;
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL flow_contract: %s\n",e.what()); return 1; }
}
