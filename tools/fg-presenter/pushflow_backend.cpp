// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): Based on PushFPS (GPL-3.0-or-later), modified 2026-11-22 for the OBS presenter.
#include "pushflow_backend.h"
#include "pushflow_compute.h"

namespace nr::fg::vfg {
namespace pf = pushflow;
namespace {
constexpr DWORD drain_ms = 750;
constexpr size_t subframes = max_multiplier - 1;
}

struct PushFlowBackend::Impl {
    const PushFlowOpen callbacks;
    pf::StartingMarker marker{pf::marker_directory(), callbacks.session.markerOwnerToken};
    pf::OrtState ort{callbacks.log};
    Gpu* const gpu;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12Resource>, content_input_count> retainedInputs;
    std::array<ComPtr<ID3D12Resource>, 2 * subframes> retainedOutputs;
    std::optional<pf::Compute> compute;
    std::array<pf::Command, subframes> packs, resolves;
    ComPtr<ID3D12Fence> done;
    Handle event;
    std::optional<FgPairTicket> ticket;
    std::array<UINT64, subframes> subfences{};
    UINT64 fence{}, pairs{}, inferences{};
    uint32_t maxMultiplier{};
    bool ready{}, queued{}, teardownStarted{}, drained{}, releaseFailed{};

    Impl(const PushFlowOpen& open, Gpu* device) : callbacks(open), gpu(device) {}

    bool drain()
    {
        if (!queued) return true;
        const auto value = ++fence;
        if (FAILED(queue->Signal(done.Get(), value)) ||
            FAILED(done->SetEventOnCompletion(value, event.h)) ||
            WaitForSingleObject(event.h, drain_ms) != WAIT_OBJECT_0) return false;
        const auto completed = done->GetCompletedValue();
        return completed != UINT64_MAX && completed >= value;
    }

    bool stop()
    {
        if (teardownStarted) return drained;
        ready = false;
        teardownStarted = true;
        const bool armed = marker.begin_teardown(callbacks.userClosing());
        const bool synced = ort.sync();
        const bool idle = drain();
        drained = armed && synced && idle;
        return drained;
    }
};

PushFlowBackend::PushFlowBackend() = default;
PushFlowBackend::~PushFlowBackend()
{
    // Production callers explicitly close inside the generation ownership
    // boundary. Never allow an unverified teardown to unwind its owner.
    if (!close()) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

bool PushFlowBackend::open(const PushFlowOpen& open, Gpu* gpu)
{
    const auto multiplier = open.preview ? open.preview->effective(open.session.multiplier) : open.session.multiplier;
    if (impl_ || faulted_ || multiplier <= 1) return false;
    impl_ = std::make_unique<Impl>(open, gpu);
    auto& p = *impl_;
    const auto markerResult = p.marker.begin();
    if (markerResult != pf::StartingMarker::Open::Armed) {
        if (markerResult == pf::StartingMarker::Open::PreviousCrash && open.preview) open.preview->quarantine();
        const char* event = markerResult == pf::StartingMarker::Open::PreviousCrash ?
            "pushflow_previous_crash_detected" : "pushflow_starting_marker_failed";
        std::fprintf(open.log, "event=%s\n", event);
        faulted_ = true;
        return false;
    }
    const auto init = [&] {
        open.beat();
        const pf::fs::path directory(open.session.pushflowRuntimeDir);
        if (!p.ort.load(directory, gpu ? gpu->d3d_device() : nullptr)) return false;
        open.beat();
        const bool quality = open.session.pushflowTier == PushFlowTier::Quality;
        const auto model = directory / L"models" / (quality ? L"pushflow-quality.onnx" : L"pushflow-fast.onnx");
        const auto facts = pf::read_model_facts(model);
        const auto geometry = veyra::pushflow::motion::makeGeometry({open.session.width, open.session.height,
            quality ? 2u : 0u, facts.pad});
        if (!geometry.valid() || !facts.valid) return false;
        p.maxMultiplier = multiplier;
        p.queue = gpu->drawQueue;
        p.retainedInputs = gpu->input;
        p.retainedOutputs = gpu->syntheticTex;
        if (!p.ort.open(model, facts, geometry, p.queue.Get(), open.beat)) return false;
        outputCount_ = p.ort.outputCount;
        shader_ = p.ort.shader;
        if (!pf::matches_outputs(open.expectedOutputs, outputCount_, shader_)) {
            std::fprintf(open.log, "event=pushflow_output_contract_mismatch expected=%u actual=%zu\n",
                open.expectedOutputs, p.ort.outputCount);
            return false;
        }
        if (!p.ort.bind(gpu->d3d_device())) return false;
        open.beat();
        p.compute.emplace(*gpu, p.ort, pf::Compute::Config{geometry, facts});
        for (auto& command : p.packs) command.prepare(gpu->d3d_device());
        for (auto& command : p.resolves) command.prepare(gpu->d3d_device());
        check(gpu->d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.done)), "PushFlow fence");
        p.event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!p.event.h) return false;
        open.beat();
        p.compute->clear_input(p.packs[0].begin());
        p.packs[0].submit(p.queue.Get());
        p.queued = true;
        const bool warm = p.ort.run() && p.ort.sync() && p.drain();
        open.beat();
        if (!warm) runtimeFault_ = true;
        return warm;
    };
    try {
        p.ready = init();
    } catch (const ipc::Error& error) {
        std::fprintf(open.log, "event=pushflow_gpu_error detail=%s\n", error.what());
        runtimeFault_ = p.queued;
    }
    if (!p.ready) {
        faulted_ = true;
        runtimeFault_ = runtimeFault_ || p.ort.poisoned();
        std::fprintf(open.log, "event=pushflow_open_fallback_1x\n");
        if (!runtimeFault_) close();
        return false;
    }
    std::fprintf(open.log, "event=pushflow_model outputs=%zu shader=%s\n", p.ort.outputCount,
        p.ort.shader == pf::ShaderPath::Unpack ? "unpack" : "reconstruct");
    return true;
}

std::optional<FgPairTicket> PushFlowBackend::submit_pair(const FgPairDesc& pair, unsigned prev, unsigned curr)
{
    auto& p = *impl_;
    if (!p.ready || faulted_ || pair.discontinuity || pair.effectiveMultiplier <= 1 ||
        pair.effectiveMultiplier > p.maxMultiplier ||
        (p.ticket && !subframe_ready(*p.ticket, p.ticket->lastSubmittedSub))) return {};
    if (prev >= p.retainedInputs.size() || curr >= p.retainedInputs.size() || prev == curr ||
        pair.prevTex != p.retainedInputs[prev].Get() || pair.currTex != p.retainedInputs[curr].Get()) return {};
    const FgPairTicket ticket{++p.pairs, prev, curr, pair.effectiveMultiplier, pair.effectiveMultiplier - 1, 0};
    p.ticket.emplace(ticket);
    uint64_t queuedAt{};
    for (uint32_t sub = 1; sub <= ticket.lastSubmittedSub; ++sub) {
        const pf::Compute::Dispatch work{ticket, sub};
        const auto reads = pf::input_reads(p.ort.shader, sub);
        const auto& pack = p.packs[sub - 1];
        p.compute->pack(pack.begin(), work);
        pack.submit(p.queue.Get());
        if (reads.pack) p.gpu->mark_external_inputs_read(prev, curr);
        if (sub == 1) {
            if (pair.onStageQueued) pair.onStageQueued();
            queuedAt = clock_ns();
        }
        if (!p.ort.run()) break;
        const auto& resolve = p.resolves[sub - 1];
        p.compute->resolve(resolve.begin(), work);
        resolve.submit(p.queue.Get());
        if (reads.resolve) p.gpu->mark_external_inputs_read(prev, curr);
        const auto value = ++p.fence;
        check(p.queue->Signal(p.done.Get(), value), "PushFlow subframe fence");
        p.subfences[sub - 1] = value;
        if (++p.inferences % pf::steady_interval == 0 && (!p.ort.sync() || !p.drain())) break;
        if (!p.marker.steady()) break;
        if (sub == ticket.lastSubmittedSub) {
            if (pair.onEnqueued) pair.onEnqueued(clock_ns() - queuedAt);
            return ticket;
        }
    }
    faulted_ = runtimeFault_ = true;
    return {};
}

bool PushFlowBackend::subframe_ready(const FgPairTicket& ticket, uint32_t sub)
{
    const auto& p = *impl_;
    if (faulted_ || !p.ticket || *p.ticket != ticket || sub == 0 || sub > ticket.lastSubmittedSub) return false;
    const auto completed = p.done->GetCompletedValue();
    if (completed == UINT64_MAX) {
        faulted_ = runtimeFault_ = true;
        return false;
    }
    return completed >= p.subfences[sub - 1];
}

void PushFlowBackend::resolve_subframe(ID3D12GraphicsCommandList* cmd, const FgPairTicket& ticket,
    ID3D12Resource* out, uint32_t sub)
{
    if (!subframe_ready(ticket, sub)) return;
    auto* source = impl_->retainedOutputs[sub - 1].Get();
    if (source == out) return; // The resolve list already returned this presenter texture to COMMON.
    constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
    constexpr auto src = D3D12_RESOURCE_STATE_COPY_SOURCE, dst = D3D12_RESOURCE_STATE_COPY_DEST;
    const D3D12_RESOURCE_BARRIER enter[]{pf::transition(source, common, src), pf::transition(out, common, dst)};
    cmd->ResourceBarrier(ARRAYSIZE(enter), enter);
    cmd->CopyResource(out, source);
    const D3D12_RESOURCE_BARRIER leave[]{pf::transition(source, src, common), pf::transition(out, dst, common)};
    cmd->ResourceBarrier(ARRAYSIZE(leave), leave);
}

void PushFlowBackend::synchronize()
{
    if (impl_ && !impl_->stop()) {
        faulted_ = runtimeFault_ = true;
        check(E_FAIL, "PushFlow synchronize/drain");
    }
}

bool PushFlowBackend::close()
{
    if (!impl_) return true;
    auto& p = *impl_;
    if (p.releaseFailed) return false;
    const bool clean = p.stop() && p.ort.release();
    const bool markers = p.marker.finish_teardown(p.callbacks.userClosing(), clean && !runtimeFault_);
    if (clean && markers) impl_.reset();
    else p.releaseFailed = true;
    if (!clean || !markers) faulted_ = runtimeFault_ = true;
    return clean && markers;
}
} // namespace nr::fg::vfg
