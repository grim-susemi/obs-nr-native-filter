// SPDX-License-Identifier: GPL-3.0-only
// Adapted from R2 DlssNr_Proxy/CompatibilityRuntime (OptiScaler NR fork).
// Sources have no copyright banner; attribution: provenance/source-notices/CREDITS.md.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "nr_contract.h"
#include "nr_lifecycle.h"
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>
namespace nr::runtime {
struct RuntimeIdentity { std::filesystem::path canonicalPath; std::string sha256; };
struct Inspection { bool accepted=false,loaded=false; std::string reason,sha256; std::vector<std::string> exports; };
Inspection inspect(const RuntimeIdentity&); // Static read-only; never executes the DLL.
Inspection inspect_pe(std::span<const unsigned char>); // Same parser; fixture seam.
Inspection inspect_driver_pe(std::span<const unsigned char>);
struct DriverRegistration {
    std::wstring deviceInstance,driverKey,publishedInf,activePackage,driverVersion,infSection;
};
DriverRegistration driver_registration(std::span<const DriverRegistration>);
std::filesystem::path published_inf_path(const std::filesystem::path& windowsInfDirectory,std::wstring_view publishedName);
struct DriverBinding {
    DriverRegistration registration;
    RuntimeIdentity identity;
    std::filesystem::path driverStoreInf;
    bool signatureVerified=false;
};
DriverBinding resolve_driver(); // Registered present NVIDIA device; metadata only, never loads CORE.
struct ImportSlot { void** address; bool wide; };
bool find_path_imports(std::span<unsigned char>,std::vector<ImportSlot>&);
enum class Route { Driver,Direct,DriverFallback };
struct Dispatch {
    decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters) allocate=nullptr;
    decltype(&NVSDK_NGX_D3D12_DestroyParameters) destroy=nullptr;
    decltype(&NVSDK_NGX_D3D12_CreateFeature) create=nullptr;
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) evaluate=nullptr;
    decltype(&NVSDK_NGX_D3D12_ReleaseFeature) release=nullptr;
    bool valid() const { return allocate && destroy && create && evaluate && release; }
};
// External boundaries for deterministic CPU execution of the same Module close
// sequence. Production leaves these empty; no coordinator is replaced.
struct RetirementControl {
    std::function<NVSDK_NGX_Result()> shutdown;
    NVSDK_NGX_Parameter* capabilities=nullptr;
    std::function<bool()> restoreImports;
    std::function<lifecycle::VendorResult()> releaseLibrary;
    lifecycle::VendorTrace* trace=nullptr;
};
// Driver table is the initialized NVIDIA NGX dispatcher on the private worker device.
// The lease owns driver/device initialization. No registry or generic provider mechanism.
class Module {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Module(std::unique_ptr<Impl>);
    friend class Pass;
public:
    ~Module();
    bool close();
    Module(const Module&)=delete;
    Module& operator=(const Module&)=delete;
    static std::shared_ptr<Module> open(const RuntimeIdentity&,Dispatch,std::shared_ptr<void> driverLease,
        ID3D12Device*,const std::filesystem::path& privateDataPath,Route,lifecycle::VendorTrace* trace=nullptr);
    // Only external NGX/completion boundaries replaced, never the production Pass.
    static std::shared_ptr<Module> controlled(Dispatch,Dispatch,std::shared_ptr<void>,Route,
        std::function<bool()> closeBoundary = {},RetirementControl = {});
};
enum class Completion { Recorded,Submitted,SignalFailed,Complete,Discarded };
struct Recording {
    ID3D12GraphicsCommandList* commands;
    uint64_t epoch;
    std::shared_ptr<void> resourceLease; // list/allocator/uploads/ALL input and output resources
    std::function<Completion()> completion;
};
// submitted AFTER ExecuteCommandLists; failed signal remains unresolved. No wait on renderer.
class Submission {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Submission();
    ~Submission();
    bool submitted(ID3D12CommandQueue*);
    bool discard(); // only before submission
    Completion state() const;
};
struct Inputs {
    ID3D12Resource *color,*depth,*motion,*output;
    Rect depthRegion,motionRegion;
    bool depthInverted,reset;
    float mvScaleX,mvScaleY;
    bool producerComplete;
};
class Pass {
    // Single worker owner serializes this pass; module mutex serializes foreign calls across passes.
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Pass(std::shared_ptr<Module>);
    ~Pass(); // unresolved ownership deliberately retained, not force-freed
    Pass(const Pass&)=delete;
    Pass& operator=(const Pass&)=delete;
    NVSDK_NGX_Result prepare(const Settings&,Extent,uint64_t historyEpoch,Recording);
    bool ready(uint64_t laterEpoch) const;
    NVSDK_NGX_Result run(const JobContract&,const Inputs&,Recording);
    std::optional<CompletedFrame> completed(const JobKey&) const;
    bool publishable(const JobKey&) const;
    void expire(const JobKey&);
    void cancel(const JobKey&);
    bool release(); // false retains module/feature/map/leases; retry only on exact completion event
};
}
