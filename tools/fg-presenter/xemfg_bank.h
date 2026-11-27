// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Per-bank XeMFG state: one provider feature, one command ring, one guide
// provider. Kept apart from the IFgBackend contract so either can be reviewed
// without the other's lifetime rules in view.
#include "anr_commands.h"
#include "anr_guides.h"
#include "ngx_parameters.h"
#include "xemfg_backend.h"
#include <exception>
#include <memory>
#include <optional>
#include <wrl/client.h>

namespace nr::fg::vfg::detail {
// Local OFA is permitted only for an explicit ConsumerFallback. Creating it at
// open would load the NR guide shader (and two extra OFA sessions) for every
// session that never needs it, so the real provider is built on first use.
class LazyLocalGuides final : public IAnrGuideProvider {
    ID3D12Device* device_;
    const Extent size_;
    const uint64_t interval_;
    FILE* const log_;
    std::unique_ptr<IAnrGuideProvider> inner_;
    IAnrGuideProvider& ensure()
    {
        if (!inner_) {
            try {
                inner_ = make_anr_guides(device_, size_, interval_);
            } catch (const std::exception& error) {
                if (log_) fprintf(log_, "event=xemfg_runtime_fault reason=local_guides message=%s\n", error.what());
                throw;
            }
        }
        return *inner_;
    }
public:
    LazyLocalGuides(ID3D12Device* device, Extent size, uint64_t interval, FILE* log)
        : device_(device), size_(size), interval_(interval), log_(log) {}
    FlowProblem submit(const FlowInput& input) override { return ensure().submit(input); }
    const FlowResult* result() const override { return inner_ ? inner_->result() : nullptr; }
    bool collect(const JobKey& key, NV_OF_FENCE_POINT fence) override
    {
        return !inner_ || inner_->collect(key, fence);
    }
    bool close() override { return !inner_ || inner_->close(); }
};

struct Bank {
    const size_t index;
    std::unique_ptr<IAnrGuideProvider> guides;
    std::unique_ptr<AnrBgra> bgra;
    std::unique_ptr<AnrCommands> commands;
    NgxParameters params;
    std::optional<FlowImage> previous;
    std::optional<JobKey> pending;
    std::optional<FgPairTicket> ticket;
    Microsoft::WRL::ComPtr<ID3D12Fence> consumeFence;
    uint64_t consumeValue{}, fence{}, lastSerial{}, sequence{}, epoch{};
    unsigned slot{};
    void* feature{};
    bool featureCreated{}, retired{true}, guidesUncertain{};

    Bank(ID3D12Device* device, ID3D12CommandQueue* queue, const XeMfgOpen& options, size_t bankIndex)
        : index(bankIndex),
          guides(make_shared_anr_guides(device, options.size, guide_interval_ns(options.frameIntervalNs),
              std::make_unique<LazyLocalGuides>(device, options.size,
                  guide_interval_ns(options.frameIntervalNs), options.log))),
          bgra(std::make_unique<AnrBgra>(device, options.size)),
          commands(std::make_unique<AnrCommands>(device, queue))
    {}
};
} // namespace nr::fg::vfg::detail
