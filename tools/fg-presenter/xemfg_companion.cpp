// SPDX-License-Identifier: GPL-3.0-only
#include "xemfg_companion.h"
#include "xefg_runtime.h"
#include <span>
#include <string>
#include <utility>

namespace nr::fg::vfg {
namespace {
class XeFgLease final : public xemfg::CompanionLease {
public:
    std::unique_ptr<XeFgRuntime> runtime;
};

class CompanionGate final : public xemfg::CompanionValidator {
    FILE* log_;
public:
    explicit CompanionGate(FILE* log) : log_(log) {}
    xemfg::CompanionResult validate(std::span<const std::wstring> candidates) override
    {
        std::string detail;
        for (const auto& candidate : candidates) {
            auto runtime = std::make_unique<XeFgRuntime>();
            const auto status = runtime->open(candidate, log_);
            if (status == XeFgLoadStatus::Ok) {
                auto lease = std::make_unique<XeFgLease>();
                lease->directory = runtime->directory();
                lease->runtime = std::move(runtime);
                return {xemfg::ValidationStatus::Accepted, {}, std::move(lease)};
            }
            detail = xefg_load_status_name(status);
            detail += ':';
            detail += runtime->detail();
            if (log_) fprintf(log_, "event=xemfg_companion_rejected directory=%ls reason=%s gate=%s\n",
                candidate.c_str(), xefg_load_status_name(status), runtime->detail());
        }
        return {xemfg::ValidationStatus::CompanionRejected,
            detail.empty() ? std::string("no_candidates") : detail, {}};
    }
};
} // namespace

std::unique_ptr<xemfg::CompanionValidator> make_companion_gate(FILE* log)
{
    return std::make_unique<CompanionGate>(log);
}
} // namespace nr::fg::vfg
