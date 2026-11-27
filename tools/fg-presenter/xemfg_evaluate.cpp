// SPDX-License-Identifier: GPL-3.0-only
// XeMFG evaluation: provider parameter population and the single list that
// converts each generated frame into Gpu::syntheticTex. Kept out of the
// IFgBackend lifecycle file so the parameter contract reviews on its own.
#include "xemfg_impl.h"
#include "anr_d3d.h"
#include "anr_ngx_keys.h"
#include <algorithm>

namespace nr::fg::vfg {
namespace k = nr::fg::anr::keys;
// The D3D12 provider reads the DLSS-G parameter names directly. The ANR key set
// covers the shared names; these are the entries only the 3x/4x route sets.
constexpr char enableInterp[] = "DLSSG.EnableInterp";
constexpr char depthBaseX[] = "DLSSG.DepthSubrectBaseX";
constexpr char depthBaseY[] = "DLSSG.DepthSubrectBaseY";
constexpr char depthWidth[] = "DLSSG.DepthSubrectWidth";
constexpr char depthHeight[] = "DLSSG.DepthSubrectHeight";
constexpr char mvecsBaseX[] = "DLSSG.MVecsSubrectBaseX";
constexpr char mvecsBaseY[] = "DLSSG.MVecsSubrectBaseY";
constexpr char mvecsWidth[] = "DLSSG.MVecsSubrectWidth";
constexpr char mvecsHeight[] = "DLSSG.MVecsSubrectHeight";
constexpr char jitterOffsetX[] = "DLSSG.JitterOffsetX";
constexpr char jitterOffsetY[] = "DLSSG.JitterOffsetY";
constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
constexpr auto shaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto copyDest = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr float nearPlane = 0.1f, farPlane = 1000.f, verticalFov = 1.5707963f;

bool XeMfgBackend::Impl::consumer_complete(const detail::Bank& bank)
{
    if (!bank.consumeFence) return true;
    const auto completed = bank.consumeFence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= bank.consumeValue;
}

detail::Bank* XeMfgBackend::Impl::free_bank()
{
    for (size_t i = 0; i < bankCount; ++i) {
        auto& bank = *banks[i];
        if (!bank.ticket) return &bank;
        if (bank.retired && bank.commands->complete(bank.fence) && consumer_complete(bank)) return &bank;
    }
    return nullptr;
}

void XeMfgBackend::Impl::drain(detail::Bank& bank)
{
    bank.commands->wait();
    if (bank.guidesUncertain) check(E_FAIL, "XeMfg guide ownership unconfirmed");
    if (!bank.pending) return;
    if (!bank.guides->collect(*bank.pending, bank.commands->last_point()))
        check(E_FAIL, "XeMfg guide collect");
    bank.pending.reset();
}

bool XeMfgBackend::Impl::create_feature(detail::Bank& bank)
{
    auto* list = bank.commands->begin();
    bank.params.Set(k::width, size.width);
    bank.params.Set(k::height, size.height);
    const bool created = runtime.create_feature(list, bank.params, bank.feature);
    bank.commands->discard(list);
    bank.commands->recycle();
    bank.featureCreated = created;
    return created;
}

bool XeMfgBackend::Impl::record_evaluate(detail::Bank& bank, ID3D12GraphicsCommandList* list,
    ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth, unsigned sub, uint64_t serial,
    unsigned count, bool reset)
{
    auto* output = gpu->syntheticTex[bank.index * (max_multiplier - 1) + sub - 1].Get();
    auto& p = bank.params;
    p.Set(k::backbuffer, color);
    p.Set(k::mvecs, motion);
    p.Set(k::depth, depth);
    p.Set(k::outputInterpolated, output);
    p.Set(k::multiFrameCount, count);
    p.Set(k::multiFrameIndex, sub);
    p.Set(k::reset, reset ? 1u : 0u);
    p.Set(k::mvecScaleX, 1.f);
    p.Set(k::mvecScaleY, 1.f);
    p.Set(k::backbufferFrameId, serial);
    p.Set(enableInterp, 1u);
    p.Set(k::depthInverted, 0u);
    p.Set(k::mvecJittered, 0u);
    p.Set(jitterOffsetX, 0.f);
    p.Set(jitterOffsetY, 0.f);
    p.Set(k::cameraAspectRatio, static_cast<float>(size.width) / static_cast<float>(size.height));
    p.Set(k::cameraNear, nearPlane);
    p.Set(k::cameraFar, farPlane);
    p.Set(k::cameraFov, verticalFov);
    for (const auto* key : {depthBaseX, depthBaseY, mvecsBaseX, mvecsBaseY}) p.Set(key, 0u);
    for (const auto* key : {depthWidth, mvecsWidth}) p.Set(key, size.width);
    for (const auto* key : {depthHeight, mvecsHeight}) p.Set(key, size.height);
    transition(list, color, common, shaderRead);
    transition(list, motion, common, copyDest);
    transition(list, depth, common, copyDest);
    transition(list, output, common, uav);
    const auto recorded = runtime.evaluate(list, bank.feature, p);
    transition(list, output, uav, common);
    transition(list, depth, copyDest, common);
    transition(list, motion, copyDest, common);
    transition(list, color, shaderRead, common);
    if (!recorded) {
        if (log) fprintf(log, "event=xemfg_runtime_fault reason=evaluate frame=%llu sub=%u\n",
            static_cast<unsigned long long>(serial), sub);
        return false;
    }
    if (submissionTrace) fprintf(submissionTrace, "xemfg frame=%llu count=%u sub=%u\n",
        static_cast<unsigned long long>(serial), count, sub);
    return true;
}

bool XeMfgBackend::Impl::evaluate_frame(detail::Bank& bank, ID3D12Resource* color, uint64_t serial,
    unsigned count, bool reset, bool shotCut)
{
    if (reset) ++bank.epoch;
    const auto dense = ++bank.sequence;
    const JobKey key{{generation, bank.epoch, dense}, bank.epoch, generation, dense};
    auto* list = bank.commands->begin();
    FlowProblem problem = FlowProblem::None;
    const FlowImage imported{key, nullptr, NV_OF_FENCE_POINT{}};
    const bool guideReset = reset || shotCut;
    const FlowResult* result = nullptr;
    if (bank.guides->shared_path(list, imported, guideReset, problem)) {
        result = bank.guides->result();
        if (problem != FlowProblem::None || !result || result->key != key) {
            if (log) fprintf(log, "event=xemfg_runtime_fault reason=shared_flow problem=%u\n",
                static_cast<unsigned>(problem));
            bank.commands->discard(list);
            return false;
        }
        bank.pending.emplace(key);
        bank.commands->enqueue_wait(result->complete);
        bank.guidesUncertain = false;
        bank.previous.reset();
    } else {
        bank.bgra->record(list, color, bank.slot);
        const auto converted = bank.commands->submit(list);
        const FlowImage image{key, bank.bgra->get(bank.slot), converted};
        const FlowInput input{image, reset ? nullptr : (bank.previous ? &*bank.previous : nullptr), size,
            {0, 0, size.width, size.height}, guideReset};
        bank.guidesUncertain = true;
        const auto status = bank.guides->submit(input);
        result = bank.guides->result();
        if (status != FlowProblem::None || !result || result->key != key) {
            if (log) fprintf(log, "event=xemfg_runtime_fault reason=guides status=%u\n",
                static_cast<unsigned>(status));
            return false;
        }
        bank.pending.emplace(key);
        list = bank.commands->begin();
        bank.commands->enqueue_wait(result->complete);
        bank.guidesUncertain = false;
        bank.previous.emplace(image);
        bank.slot ^= 1;
    }
    for (unsigned sub = 1; sub <= count; ++sub) {
        if (!record_evaluate(bank, list, color, result->motion, result->depth, sub, serial, count, guideReset)) {
            bank.commands->discard(list);
            return false;
        }
    }
    bank.fence = bank.commands->submit(list).value;
    bank.lastSerial = serial;
    return true;
}
} // namespace nr::fg::vfg
