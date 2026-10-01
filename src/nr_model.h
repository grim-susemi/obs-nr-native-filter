// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "frame_pipeline.h"
#include "nr_color.h"
#include "nr_core.h"
#include "nr_guides.h"
#include "nr_runtime.h"
#include <atomic>

namespace nr::model {
struct Config {
    runtime::RuntimeIdentity runtime;
    std::filesystem::path driver, data;
    std::vector<std::byte> codec, color, guides;
};
// Graphics-owned immutable copies, never a Bridge raw slot or an OBS borrow.
struct Frame {
    Microsoft::WRL::ComPtr<ID3D12Resource> original, analysis;
    Microsoft::WRL::ComPtr<ID3D12Fence> producerLease;
    const std::shared_ptr<void> graphicsLease;
    const NV_OF_FENCE_POINT producer;
    Frame(ID3D12Resource*,ID3D12Resource*,std::shared_ptr<void>,NV_OF_FENCE_POINT);
};
enum class Fault { None, Capability, Creation, Evaluation, Submission };
struct Job {
    struct Resources;
    const std::shared_ptr<const Frame> frame;
    const JobContract contract;
    const ColorExposure exposure;
    const Fault fault;
    const HANDLE observed, gpu;
    std::atomic<bool> ready{false};
    std::atomic<bool> published{false};
    bool created=false, evaluated=false, composed=false, submitted=false, eligible=false;
    uint32_t code=0, invalid=0;
    uint64_t epoch=0;
    uint64_t creationEpoch=0;
    uint32_t driverCreateCode=0;
    bool driverHandle=false;
    uintptr_t runtimeModule=0;
    std::wstring modulePath;
    std::string diagnostic;
    std::array<int64_t,8> chronology{}; // create/encode/evaluate/compose/execute/submitted/complete/status
    std::optional<FlowResult> flow;
    ID3D12Resource* output=nullptr; // Borrowed from this job's retained Bridge.
    NV_OF_FENCE_POINT completion() const;
    std::array<ID3D12Resource*,5> color_resources() const;
    std::array<uintptr_t,3> descriptor_heaps() const;
    ID3D12Resource* invalid_status_resource() const;
    // Published by ready's release store, never read while worker writes.
    std::shared_ptr<Resources> resources;
    explicit Job(std::shared_ptr<const Frame>,const JobContract&,ColorExposure,Fault=Fault::None);
    ~Job();
    Job(const Job&)=delete;
    Job& operator=(const Job&)=delete;
};
class Processor final : public pipeline::Processor {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Processor(std::shared_ptr<interop::Context>,Config,uint64_t instance);
    ~Processor() override;
    pipeline::Processed process(const pipeline::Work&,std::stop_token) override;
    void worker_stopping() noexcept override;
    bool closed() const;
    uint64_t applied() const;
    bool publish(const std::shared_ptr<Job>&,const Selection&,const interop::Ownership&);
};
} // namespace nr::model
