// SPDX-License-Identifier: GPL-3.0-only
// Driver-reset naming: a running device removed reason and a later filter thrown HRESULT both read as device lost.
#include "interop.h"
#include <cstdio>
#include <stdexcept>
namespace {
int failures=0;
void expect(bool ok,const char* what){if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);++failures;}}
}
int main() {
 using namespace nr::interop;
 expect(device_lost_cause(nullptr,HResultError(DXGI_ERROR_DEVICE_HUNG,"hung"))==DXGI_ERROR_DEVICE_HUNG,"thrown DEVICE_HUNG names the reset");
 expect(device_lost_cause(nullptr,HResultError(DXGI_ERROR_DEVICE_REMOVED,"removed"))==DXGI_ERROR_DEVICE_REMOVED,"thrown DEVICE_REMOVED names the reset");
 expect(device_lost_cause(nullptr,HResultError(E_OUTOFMEMORY,"oom"))==S_OK,"other HRESULTs are not a reset");
 expect(device_lost_cause(nullptr,std::runtime_error("plain"))==S_OK,"untyped failures are not a reset");
 Microsoft::WRL::ComPtr<ID3D11Device> d11;
 if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,nullptr))){std::fprintf(stderr,"FAIL: no hardware D3D11 device\n");return 1;}
 const auto first=Context::create(d11.Get());
 expect(device_lost_cause(first->device(),std::runtime_error("x"))==S_OK,"a live device is not a reset");
 expect(dred_report(first->device(),first->queue()).empty(),"DRED stays off without OBS_NR_DRED");
 Microsoft::WRL::ComPtr<ID3D12Device5> d5;
 if(FAILED(first->device()->QueryInterface(IID_PPV_ARGS(&d5)))){std::fprintf(stderr,"FAIL: ID3D12Device5 unavailable\n");return 1;}
 d5->RemoveDevice();
 expect(device_lost_cause(first->device(),std::runtime_error("x"))!=S_OK,"a removed device reads as a reset");
 // A filter created afterwards: D3D12 returns the same removed device, or creation fails with a device-lost code.
 try {
  const auto later=Context::create(d11.Get());
  expect(same_identity(later->device(),first->device()),"a later context gets the same removed device");
  expect(device_lost_cause(later->device(),std::runtime_error("x"))!=S_OK,"a later context reads as a reset");
 } catch(const std::exception& e) {
  expect(device_lost_cause(nullptr,e)!=S_OK,"a later context creation failure names the reset");
 }
 if(failures==0)std::puts("device_reset: PASS");
 return failures==0?0:1;
}
