// SPDX-License-Identifier: GPL-3.0-only
#include "interop.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace nr::interop;
static unsigned checks=0;
static void expect(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
int main(){try{
 const auto max=std::numeric_limits<uint64_t>::max();
 expect(completion(7,6)==Completion::Pending,"unfinished fence completed");
 expect(completion(7,7)==Completion::Complete,"exact fence rejected");
 expect(completion(7,8)==Completion::Complete,"later completion rejected");
 expect(completion(7,max)==Completion::Failed,"UINT64_MAX admitted");
 expect(completion(0,max)==Completion::Failed,"failed unused fence admitted");
 expect(completion(0,0)==Completion::Complete,"no dependency rejected");
 FenceCursor cursor;expect(cursor.reserve()==1&&cursor.reserve()==2,"nonmonotonic reservation");
 FenceCursor exhausted(max-1);expect(!exhausted.reserve(),"failed-sentinel value reserved");
 const nr::JobKey key{{11,3,8},5,9,20},foreign{{11,3,8},5,9,21};
 Ownership x{key,WorkPhase::Submitted,1,2,3,1,2,3,false,false,false};
 expect(reusable(x)&&publishable(x,key),"complete ownership rejected");
 expect(!publishable(x,foreign),"stale ticket published");
 for(unsigned mask=0;mask<8;++mask){
  auto y=x;y.producerDone=(mask&1)?1:0;y.workerDone=(mask&2)?2:0;y.consumerDone=(mask&4)?3:0;
  expect(reusable(y)==(mask==7),"physical dependencies ignored");
  expect(publishable(y,key)==((mask&3)==3),"renderer readiness depends on wrong fence");
 }
 x.consumerOpen=true;expect(!reusable(x),"open consumer released");x.consumerOpen=false;
 x.cancelled=true;expect(!publishable(x,key)&&reusable(x),"cancel frees running ownership or publishes");
 x.workerDone=0;expect(!reusable(x),"cancel released unfinished worker");
 x.phase=WorkPhase::Discarded;x.worker=0;expect(reusable(x),"discarded recording retained");
 x.phase=WorkPhase::Recording;expect(!reusable(x),"unsubmitted recording released");
 x.phase=WorkPhase::Submitted;x.worker=2;x.workerDone=max;
 expect(!reusable(x)&&!publishable(x,key),"failed worker fence released");
 x.workerDone=2;x.consumerDone=max;expect(!reusable(x)&&!publishable(x,key),"failed consumer admitted");
 x.consumerDone=3;x.failed=true;expect(!reusable(x),"signal failure released ownership");
 expect(same_luid({7,2},{7,2})&&!same_luid({7,2},{8,2})&&!same_luid({7,2},{7,3}),"adapter identity mismatch");
 expect(canonical_format(nr::PixelFormat::Bgra8)==DXGI_FORMAT_R8G8B8A8_UNORM,"BGRA illegal-copy policy");
 expect(canonical_format(nr::PixelFormat::Rgba16Float)==DXGI_FORMAT_R16G16B16A16_FLOAT,"HDR format lost");
 expect(source_format_compatible(DXGI_FORMAT_B8G8R8A8_TYPELESS,nr::PixelFormat::Bgra8),"actual OBS BGRA typeless capture regression");
 expect(source_format_compatible(DXGI_FORMAT_R8G8B8A8_TYPELESS,nr::PixelFormat::Rgba8),"actual OBS RGBA typeless capture regression");
 expect(source_format_compatible(DXGI_FORMAT_R16G16B16A16_TYPELESS,nr::PixelFormat::Rgba16Float),"FP16 typed family rejected");
 expect(source_format_compatible(DXGI_FORMAT_R10G10B10A2_TYPELESS,nr::PixelFormat::Rgb10A2),"RGB10 typed family rejected");
 expect(!source_format_compatible(DXGI_FORMAT_R8G8B8A8_TYPELESS,nr::PixelFormat::Bgra8),"illegal BGRA/RGBA CopyResource admitted");
 expect(!source_format_compatible(DXGI_FORMAT_UNKNOWN,nr::PixelFormat::Unknown),"unknown source format admitted");
 std::printf("PASS interop_contract %u checks",checks);std::puts("");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"FAIL interop_contract: %s",e.what());return 1;}}
