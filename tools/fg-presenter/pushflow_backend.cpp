// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): Based on PushFPS (GPL-3.0-or-later), modified 2026-11-22 for the OBS presenter.
#include "pushflow_backend.h"
#include "pushflow_compute.h"
#include <utility>

namespace nr::fg::vfg {
namespace pf = pushflow;
namespace {
constexpr DWORD drain_ms = 750;
constexpr size_t subframes = max_multiplier - 1;

template <class Fn>
struct OnScopeExit {
    Fn fn;
    explicit OnScopeExit(Fn f) noexcept : fn(std::move(f)) {}
    OnScopeExit(const OnScopeExit&) = delete;
    OnScopeExit& operator=(const OnScopeExit&) = delete;
    ~OnScopeExit() noexcept { fn(); }
};
template <class Fn>
OnScopeExit(Fn) -> OnScopeExit<Fn>;
}

struct PushFlowBackend::Impl {
    // A pair bank owns its command allocators, subframe completion fences and
    // consumer completion point. The bank is reusable only after both the
    // producer subframe fences and the consumer draws that read the outputs have
    // completed on the GPU; retired stays true until a new pair claims it.
    struct PairBank {
        std::array<pf::Command, subframes> packs, resolves;
        std::array<UINT64, subframes> subfences{};
        std::optional<FgPairTicket> ticket;
        ComPtr<ID3D12Fence> consumeFence;
        UINT64 consumeValue{};
        bool retired{};
    };
    const PushFlowOpen callbacks;
    pf::StartingMarker marker{pf::marker_directory(), callbacks.session.markerOwnerToken};
    pf::OrtState ort{callbacks.log};
    Gpu* const gpu;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12Resource>, content_input_count> retainedInputs;
    std::array<ComPtr<ID3D12Resource>, 2 * subframes> retainedOutputs;
    std::optional<pf::Compute> compute;
    std::array<PairBank, batch_capacity> banks;
    ComPtr<ID3D12Fence> done;
    Handle event;
    UINT64 fence{}, pairs{}, inferences{};
    uint32_t maxMultiplier{};
    bool ready{}, queued{}, teardownStarted{}, drained{}, releaseFailed{};
    bool failNextSubmit{};

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

    bool bank_available(size_t index) const
    {
        const auto& bank = banks[index];
        if (!bank.ticket) return true;
        if (!bank.retired || !done) return false;
        const auto completed = done->GetCompletedValue();
        if (completed == UINT64_MAX) return false;
        for (const auto value : bank.subfences) {
            if (value > completed) return false;
        }
        if (bank.consumeFence) {
            const auto consumed = bank.consumeFence->GetCompletedValue();
            if (consumed == UINT64_MAX || consumed < bank.consumeValue) return false;
        }
        return true;
    }

    // Returning a claimed bank also clears its ticket/fence bookkeeping so the
    // existing availability predicate accepts it again.
    void release_bank(PairBank& bank) noexcept
    {
        bank.ticket.reset();
        bank.consumeFence.Reset();
        bank.consumeValue = 0;
        bank.subfences.fill(0);
        bank.retired = true;
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
        for (auto& bank : p.banks) {
            for (auto& command : bank.packs) command.prepare(gpu->d3d_device());
            for (auto& command : bank.resolves) command.prepare(gpu->d3d_device());
        }
        check(gpu->d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.done)), "PushFlow fence");
        p.event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!p.event.h) return false;
        open.beat();
        p.compute->clear_input(p.banks[0].packs[0].begin());
        p.banks[0].packs[0].submit(p.queue.Get());
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
        pair.effectiveMultiplier > p.maxMultiplier) return {};
    if (prev >= p.retainedInputs.size() || curr >= p.retainedInputs.size() || prev == curr ||
        pair.prevTex != p.retainedInputs[prev].Get() || pair.currTex != p.retainedInputs[curr].Get()) return {};
    if (p.done->GetCompletedValue() == UINT64_MAX) {
        faulted_ = runtimeFault_ = true;
        return {};
    }
    size_t bankIndex = p.banks.size();
    for (size_t b = 0; b < p.banks.size(); ++b) {
        if (p.bank_available(b)) {
            bankIndex = b;
            break;
        }
    }
    if (bankIndex == p.banks.size()) return {};
    auto& bank = p.banks[bankIndex];
    const FgPairTicket ticket{++p.pairs, prev, curr, pair.effectiveMultiplier, pair.effectiveMultiplier - 1,
        static_cast<uint32_t>(bankIndex)};
    bank.ticket.emplace(ticket);
    bank.retired = false;
    bank.consumeFence.Reset();
    bank.consumeValue = 0;
    bank.subfences.fill(0);
    bool handedOff{};
    const auto restore_bank = [&] {
        if (handedOff) return;
        handedOff = true;
        // A partial submit may already have queued pack work for this bank.
        // Drain it before returning the bank; a timed-out drain keeps the fault
        // so no later pair can reuse a bank the GPU may still be reading.
        if (!faulted_ && !p.drain()) faulted_ = runtimeFault_ = true;
        p.release_bank(bank);
    };
    [[maybe_unused]] OnScopeExit restore_on_exit{restore_bank};
    uint64_t queuedAt{};
    for (uint32_t sub = 1; sub <= ticket.lastSubmittedSub; ++sub) {
        const pf::Compute::Dispatch work{ticket, sub};
        const auto reads = pf::input_reads(p.ort.shader, sub);
        const auto& pack = bank.packs[sub - 1];
        p.compute->pack(pack.begin(), work);
        pack.submit(p.queue.Get());
        if (reads.pack) p.gpu->mark_external_inputs_read(prev, curr);
        if (sub == 1) {
            if (pair.onStageQueued) pair.onStageQueued();
            queuedAt = clock_ns();
        }
        if (p.failNextSubmit) {
            p.failNextSubmit = false;
            break;
        }
        if (!p.ort.run()) break;
        const auto& resolve = bank.resolves[sub - 1];
        p.compute->resolve(resolve.begin(), work);
        resolve.submit(p.queue.Get());
        if (reads.resolve) p.gpu->mark_external_inputs_read(prev, curr);
        const auto value = ++p.fence;
        check(p.queue->Signal(p.done.Get(), value), "PushFlow subframe fence");
        bank.subfences[sub - 1] = value;
        if (++p.inferences % pf::steady_interval == 0 && (!p.ort.sync() || !p.drain())) break;
        if (!p.marker.steady()) break;
        if (sub == ticket.lastSubmittedSub) {
            if (pair.onEnqueued) pair.onEnqueued(clock_ns() - queuedAt);
            handedOff = true;
            return ticket;
        }
    }
    faulted_ = runtimeFault_ = true;
    return {};
}

bool PushFlowBackend::subframe_ready(const FgPairTicket& ticket, uint32_t sub)
{
    const auto& p = *impl_;
    if (faulted_ || ticket.slotParity >= p.banks.size() || sub == 0 || sub > ticket.lastSubmittedSub) return false;
    const auto& bank = p.banks[ticket.slotParity];
    if (!bank.ticket || *bank.ticket != ticket) return false;
    const auto completed = p.done->GetCompletedValue();
    if (completed == UINT64_MAX) {
        faulted_ = runtimeFault_ = true;
        return false;
    }
    return completed >= bank.subfences[sub - 1];
}

bool PushFlowBackend::wait_subframe(const FgPairTicket& ticket, uint32_t sub, DWORD timeoutMs)
{
    if (!impl_ || faulted_ || !impl_->done || ticket.slotParity >= impl_->banks.size() || sub == 0 ||
        sub > ticket.lastSubmittedSub) return false;
    auto& p = *impl_;
    const auto& bank = p.banks[ticket.slotParity];
    if (!bank.ticket || *bank.ticket != ticket) return false;
    const auto value = bank.subfences[sub - 1];
    if (!value) return false;
    const auto completed = p.done->GetCompletedValue();
    if (completed == UINT64_MAX) {
        faulted_ = runtimeFault_ = true;
        return false;
    }
    if (completed >= value) return subframe_ready(ticket, sub);
    Handle event;
    event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event.h) return false;
    if (FAILED(p.done->SetEventOnCompletion(value, event.h))) {
        faulted_ = runtimeFault_ = true;
        return false;
    }
    return WaitForSingleObject(event.h, timeoutMs) == WAIT_OBJECT_0 && subframe_ready(ticket, sub);
}

bool PushFlowBackend::bank_available(size_t index) const
{
    return impl_ && index < impl_->banks.size() && impl_->bank_available(index);
}

void PushFlowBackend::fail_next_submit_for_test() noexcept
{
    if (impl_) impl_->failNextSubmit = true;
}

void PushFlowBackend::resolve_subframe(ID3D12GraphicsCommandList* cmd, const FgPairTicket& ticket,
    ID3D12Resource* out, uint32_t sub)
{
    if (!subframe_ready(ticket, sub)) return;
    auto* source = impl_->retainedOutputs[ticket.slotParity * subframes + (sub - 1)].Get();
    if (source == out) return; // The resolve list already returned this presenter texture to COMMON.
    constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
    constexpr auto src = D3D12_RESOURCE_STATE_COPY_SOURCE, dst = D3D12_RESOURCE_STATE_COPY_DEST;
    const D3D12_RESOURCE_BARRIER enter[]{pf::transition(source, common, src), pf::transition(out, common, dst)};
    cmd->ResourceBarrier(ARRAYSIZE(enter), enter);
    cmd->CopyResource(out, source);
    const D3D12_RESOURCE_BARRIER leave[]{pf::transition(source, src, common), pf::transition(out, dst, common)};
    cmd->ResourceBarrier(ARRAYSIZE(leave), leave);
}

void PushFlowBackend::retire_pair(const FgPairTicket& ticket, ID3D12Fence* consumeFence, uint64_t consumeValue) noexcept
{
    if (!impl_) return;
    auto& p = *impl_;
    if (ticket.slotParity >= p.banks.size()) return;
    auto& bank = p.banks[ticket.slotParity];
    if (!bank.ticket || *bank.ticket != ticket) return;
    bank.consumeFence = consumeFence;
    bank.consumeValue = consumeValue;
    bank.retired = true;
}

void PushFlowBackend::synchronize()
{
    if (impl_ && !impl_->stop()) {
        faulted_ = runtimeFault_ = true;
        check(E_FAIL, "PushFlow synchronize/drain");
    }
}

bool PushFlowBackend::close() noexcept
{
    if (!impl_) return true;
    auto& p = *impl_;
    if (p.releaseFailed) return false;
    try {
        const bool clean = p.stop() && p.ort.release();
        const bool markers = p.marker.finish_teardown(p.callbacks.userClosing(), clean && !runtimeFault_);
        if (clean && markers) impl_.reset();
        else p.releaseFailed = true;
        if (!clean || !markers) faulted_ = runtimeFault_ = true;
        return clean && markers;
    } catch (...) {
        p.releaseFailed = true;
        faulted_ = runtimeFault_ = true;
        return false;
    }
}
} // namespace nr::fg::vfg
