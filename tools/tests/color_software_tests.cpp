// SPDX-License-Identifier: GPL-3.0-only
#include "nr_color_scenario.h"
#include <dxgi1_6.h>
#include <fstream>
#include <cstdio>
#include <vector>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("expected shader path");
        std::ifstream f(argv[1],std::ios::binary|std::ios::ate);
        if(!f||f.tellg()<=0) throw std::runtime_error("missing shader");
        const auto size=f.tellg();std::vector<std::byte> bytes(static_cast<size_t>(size));
        f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),size);
        if(!f) throw std::runtime_error("shader read failed");
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))||
            FAILED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))))
            throw std::runtime_error("explicit WARP device unavailable");
        ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))) throw std::runtime_error("WARP queue unavailable");
        const auto proof=nr::qa::run_color_scenario(device.Get(),queue.Get(),bytes);
        std::printf("PASS WARP_ONLY color software cases=%u pixels=%u normalized_codes=%g linear_half_ulps=%u; resources retired\n",
            proof.cases,proof.pixels,proof.maxNormalizedCodes,proof.maxLinearHalfUlps);return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
