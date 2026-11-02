// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "obs_native_output.h"
#include <ostream>
#include <span>
namespace nr::qa {
struct InstanceTick {
    const IngressSample input;
    ID3D11Texture2D* source;
    ID3D11Texture2D* target;
    // Caller-owned sink returns THIS prepared envelope from reserve(). Its event
    // and module-owned returned signal exist before tick dispatch, and its images
    // use the existing host observation destinations. Sink outlives both workers.
    model::Observation* observation;
};
struct InstanceTickPair { const InstanceTick a,b; };
// Body only: caller supplies prepared actual model NativeOutputs, distinct immutable
// fixtures and sinks. No initialization, fixture allocation, stop, or close here.
bool run_two_instances(obs_adapter::NativeOutput&,obs_adapter::NativeOutput&,
    std::span<const InstanceTickPair>,std::ostream&);
// Same existing ObservationSink seam as the accepted ghost host. Destinations
// are cloned from an actual completed ordinary job, never inferred mock formats.
class InstanceObservations final:public model::ObservationSink {
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    std::array<D3D12_RESOURCE_DESC,model::Observation::Count> descriptors_{};
    const std::array<std::shared_ptr<model::QaSignal>,3> signals_;
    std::array<model::Observation,6> observations_;
    size_t armed_=6;
    size_t withheld_=6;
    std::optional<JobKey> withheldKey_;
public:
    InstanceObservations(ID3D12Device*,const model::Job&,
        std::array<std::shared_ptr<model::QaSignal>,3>);
    ~InstanceObservations() override;
    void arm(size_t);
    void withhold_publication(size_t);
    void release_publication();
    model::Observation& at(size_t);
    bool publication_available(const JobKey&) const noexcept override;
    model::Observation* reserve(const IngressSample&,const pipeline::TickPlan&) noexcept override;
    void capture(model::Observation*,ID3D11Texture2D*,ID3D11Texture2D*,ID3D11Texture2D*) noexcept override {}
};
// Event-only envelopes for the existing physical CopyProcessor. No model-image
// destinations, alternate provider, inferred formats or publication predicates.
class CapacityObservations final:public model::ObservationSink {
    const std::array<std::shared_ptr<model::QaSignal>,3> signals_;
    std::array<model::Observation,80> observations_;
    size_t next_=0,armed_=80;
public:
    explicit CapacityObservations(std::array<std::shared_ptr<model::QaSignal>,3>);
    ~CapacityObservations() override;
    size_t arm();
    model::Observation& at(size_t);
    model::Observation* reserve(const IngressSample&,const pipeline::TickPlan&) noexcept override;
    void capture(model::Observation*,ID3D11Texture2D*,ID3D11Texture2D*,ID3D11Texture2D*) noexcept override {}
};
}
