// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <d3d12.h>
#include <nvOpticalFlowD3D12.h>
#include <memory>
#include <array>
#include <vector>
namespace nr {
struct FlowVector { float x,y; };
FlowVector decode_flow(int16_t,int16_t);
std::optional<uint32_t> smallest_flow_grid(std::span<const uint32_t>);
bool flow_geometry_valid(Extent,Rect);
bool flow_cost_format_valid(DXGI_FORMAT);
enum class FlowProblem { None,InvalidKey,MissingPrevious,ResetPair,KeyMismatch,Busy,InvalidResource,ApiFailure,DeviceRemoved };
FlowProblem flow_pair_problem(const JobKey&,const JobKey*,bool reset);
FlowProblem flow_chronology_problem(const JobKey&,const JobKey* previous,const JobKey* last,
    Rect active,const Rect* lastActive,bool reset);
NV_OF_INIT_PARAMS flow_init_params(Extent,uint32_t,NV_OF_PERF_LEVEL perf=NV_OF_PERF_LEVEL_MEDIUM);
NV_OF_INIT_PARAMS flow_bootstrap_params(Extent,uint32_t,NV_OF_PERF_LEVEL perf=NV_OF_PERF_LEVEL_MEDIUM);
NV_OF_INIT_PARAMS flow_refinement_params(Extent,uint32_t,uint32_t hintGrid,NV_OF_PERF_LEVEL perf=NV_OF_PERF_LEVEL_MEDIUM);
// Automatic estimator cost from working size and OBS frame interval; 0 interval = unknown -> MEDIUM, finest grid.
struct FlowPolicy { NV_OF_PERF_LEVEL perf=NV_OF_PERF_LEVEL_MEDIUM; uint32_t minGrid=1; };
FlowPolicy flow_policy(Extent,uint64_t frameIntervalNs);
std::optional<uint32_t> smallest_hint_grid(std::span<const uint32_t>,uint32_t outputGrid);
// Built by the production adapter; unit tests inspect the exact SDK submission packet.
NV_OF_EXECUTE_INPUT_PARAMS_D3D12 flow_execute_input(NvOFGPUBufferHandle current,NvOFGPUBufferHandle previous,
    bool disableTemporal,std::span<NV_OF_FENCE_POINT> dependencies,NvOFGPUBufferHandle hint=nullptr);
struct FlowModuleIdentity { std::wstring path,signer; std::string sha256; uint32_t maxApi=0; };
struct FlowCapabilities {
    std::vector<uint32_t> grids;
    std::vector<DXGI_FORMAT> inputFormats,outputFormats,costFormats;
    std::vector<uint32_t> hintSupport,hintGrids;
    std::vector<DXGI_FORMAT> hintFormats,globalFormats;
};
struct FlowImage {
    const JobKey key;
    ID3D12Resource* const analysis; // OWNED BGRA8 full working-size, not original HDR/raw.
    const NV_OF_FENCE_POINT ready; // Caller retains immutable input through physical completion.
};
struct FlowInput {
    const FlowImage current;
    const FlowImage* const previous;
    const Extent working;
    const Rect active; // Coordinates already in working pixels: scale is exactly one.
    const bool reset; // Explicit cold/reset only; never infer reset from failed OFA.
};
struct FlowResult {
    const JobKey key;
    ID3D12Resource* const motion; // R16G16_FLOAT, current -> previous, working pixel displacement.
    ID3D12Resource* const depth;  // R32_FLOAT neutral1, NOT geometric depth.
    ID3D12Resource* const cost;   // Queried R8_UINT or R32_UINT; NOT confidence/probability.
    ID3D12Resource* const hardwareVectors; // Raw S10.5 grid, diagnostic GPU consumer only; null on reset.
    const NV_OF_FENCE_POINT complete;
    const bool reset;
    ID3D12Resource* const bootstrapVectors; // Diagnostic only, never published local MV.
    ID3D12Resource* const globalVector;     // One actual forward OFA S10.5 vector.
    ID3D12Resource* const externalHints;    // Exact native global broadcast for THIS key, not final estimate.
    const NV_OF_FENCE_POINT inputAdmitted,bootstrapComplete,predictorComplete,refinementComplete;
    const NV_OF_FENCE_POINT currentProducer,previousProducer;
    ID3D12Resource* const bootstrapCurrent;
    ID3D12Resource* const bootstrapPrevious;
    const std::array<uintptr_t,10> registeredIdentities;
};
// Single worker-thread owner, one in-flight job. Caller supplies its PRIVATE device/direct queue.
// No device construction here. Caller must retain this object on failed close/collect.
// Outputs may not be reused until collect sees both physical worker AND consumer completion.
class D3D12Guides {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    D3D12Guides(ID3D12Device*,ID3D12CommandQueue*,Extent,std::span<const std::byte> shader,uint64_t frameIntervalNs=0);
    ~D3D12Guides();
    D3D12Guides(const D3D12Guides&)=delete;
    D3D12Guides& operator=(const D3D12Guides&)=delete;
    FlowProblem submit(const FlowInput&);
    const FlowResult* result() const;
    bool collect(const JobKey&,NV_OF_FENCE_POINT consumer); // Exact key; never wait/poll; UINT64_MAX is failure.
    // After collect, caller destroys its registered analysis allocations BEFORE close/session destruction.
    bool close(); // Worker-only vendor destroy after physical completion, never render thread.
    const FlowModuleIdentity& module_identity() const;
    const FlowCapabilities& capabilities() const;
    uint32_t grid_size() const;
    uint32_t state_bits() const; // N23 diagnostics: 1 pending, 2 uncertain, 4 output
    uint32_t hint_grid_size() const;
    NV_OF_PERF_LEVEL perf_level() const; // estimator speed chosen by flow_policy
    DXGI_FORMAT cost_format() const;
    // Worker-only copied descriptors of actual owned allocations, including cold
    // reset. Descriptors are not flow pixels or an ordinary-guide positive.
    std::array<D3D12_RESOURCE_DESC,2> observation_descriptors() const;
};
}
