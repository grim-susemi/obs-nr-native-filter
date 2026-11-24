// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "pushflow_fixture.h"
#include "../fg-presenter/pushflow_compute.h"
#include "../fg-presenter/pushflow_tensor_contract.h"
#include "../fg-presenter/vfg_generation_scope.h"
#include "../fg-presenter/vfg_content.h"
#include <stdexcept>

namespace nr::fg::qa {
inline void pair_dispatch_controls(TestRun& test)
{
    using namespace vfg;
    struct Backend final : IFgBackend {
        unsigned submitted{};
        bool lastCut{};
        const char* name() const noexcept override { return "fixture"; }
        unsigned version_tag() const noexcept override { return 0; }
        bool faulted() const noexcept override { return false; }
        std::optional<FgPairTicket> submit_pair(const FgPairDesc& pair, unsigned prev, unsigned curr) override
        {
            ++submitted;
            lastCut = pair.shotCut;
            return FgPairTicket{submitted, prev, curr, pair.effectiveMultiplier, pair.effectiveMultiplier - 1, 0};
        }
        bool subframe_ready(const FgPairTicket&, uint32_t) override { return false; }
        void resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource*, uint32_t) override {}
        void synchronize() override {}
    };
    Backend cutBackend, normalBackend, disconnectedBackend;
    const FgPairDesc cut{nullptr, nullptr, 1, 2, 3, false, true};
    const FgPairDesc normal{nullptr, nullptr, 2, 3, 3, false, false};
    const FgPairDesc disconnectedCut{nullptr, nullptr, 3, 5, 3, true, true};
    const FgPairDesc disconnected{nullptr, nullptr, 3, 5, 3, true, false};
    const auto cutTicket = cutBackend.try_submit_pair(cut, 0, 1);
    test.expect(cutTicket && cutTicket->lastSubmittedSub == 2 && cutBackend.submitted == 1 && cutBackend.lastCut,
        "continuous cut dispatches its own shot reset to the backend");
    const auto normalTicket = normalBackend.try_submit_pair(normal, 1, 2);
    test.expect(normalTicket && normalBackend.submitted == 1 && !normalBackend.lastCut,
        "continuous non-cut dispatches without shot reset");
    test.expect(!disconnectedBackend.try_submit_pair(disconnectedCut, 0, 1) &&
        !disconnectedBackend.try_submit_pair(disconnected, 0, 1) && disconnectedBackend.submitted == 0,
        "transport discontinuity suppresses cut and non-cut dispatch");
}

inline void output_metadata_controls(TestRun& test)
{
    using namespace vfg::pushflow;
    constexpr auto fp32 = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    const Shape input{1, 7, 192, 320};
    test.expect(compatible_output({fp32, 4, {1, 3, 192, 320}}, OutputLayout::Rgb, input) &&
        compatible_output({fp32, 4, {1, 192, 320, 2}}, OutputLayout::Grid, input) &&
        compatible_output({fp32, 4, {1, 1, 192, 320}}, OutputLayout::Mask, input),
        "concrete RGB NCHW, grid NHWC and mask NCHW output metadata");
    test.expect(!compatible_output({ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, 4, {1, 3, 192, 320}},
        OutputLayout::Rgb, input), "FP16 RGB rejected despite correct layout");
    test.expect(!compatible_output({fp32, 3, {1, 3, 192, 320}}, OutputLayout::Rgb, input),
        "rank three rejected before binding");
    test.expect(!compatible_output({fp32, 4, {1, 2, 192, 320}}, OutputLayout::Grid, input) &&
        !compatible_output({fp32, 4, {1, 1, 320, 192}}, OutputLayout::Mask, input),
        "same-size grid NCHW and transposed mask rejected");
    test.expect(compatible_output({fp32, 4, {-1, 3, -1, -1}, {"batch", "", "height", "width"}},
        OutputLayout::Rgb, input) &&
        compatible_output({fp32, 4, {1, -1, -1, 2}, {"", "pushflow_h", "pushflow_w", ""}}, OutputLayout::Grid, input) &&
        !compatible_output({fp32, 4, {1, -1, -1, 2}, {"", "pushflow_w", "pushflow_h", ""}}, OutputLayout::Grid, input) &&
        !compatible_output({fp32, 4, {1, -1, 320, 2}}, OutputLayout::Grid, input),
        "symbolic output dimensions require approved names on the right axes");
}

inline void generation_shutdown_controls(TestRun& test)
{
    struct SignalFailed {};
    for (const bool userClosed : {false, true}) {
        bool ownsGpu{}, drainSawOwner{}, caught{};
        unsigned drains{};
        struct Owner {
            bool& alive;
            explicit Owner(bool& flag) : alive(flag) { alive = true; }
            ~Owner() { alive = false; }
        };
        try {
            Owner owner(ownsGpu);
            bool submitted{};
            vfg::run_generation([&] {
                submitted = true;
                throw SignalFailed{};
            }, [&](bool fault) {
                ++drains;
                drainSawOwner = ownsGpu && submitted && fault;
                return userClosed;
            });
        } catch (const SignalFailed&) {
            caught = true;
        }
        test.expect(drainSawOwner && drains == 1 && !ownsGpu && caught != userClosed, userClosed ?
            "post-submit signal failure still drains during user close" :
            "post-submit signal failure drains before unwinding owners");
    }
    bool drained{};
    vfg::run_generation([] {}, [&](bool fault) { drained = !fault; return false; });
    test.expect(drained, "normal generation uses the same shutdown boundary");
    bool nativeDrain{}, nativeOriginal{};
    try {
        vfg::run_generation([] { throw SignalFailed{}; }, [&](bool) -> bool {
            nativeDrain = true;
            throw std::runtime_error("drain masks native fault");
        }, false);
    } catch (const SignalFailed&) {
        nativeOriginal = true;
    }
    test.expect(nativeOriginal && !nativeDrain, "native work fault reaches worker without masking drain");
}

inline void ownership_and_timeout_controls(TestRun& test, const std::filesystem::path& root)
{
    using namespace vfg;
    using namespace vfg::pushflow;
    for (unsigned mult : {2u, 3u, 4u}) {
        for (const auto path : {ShaderPath::Unpack, ShaderPath::Reconstruct}) {
            unsigned packs{}, resolves{};
            for (unsigned sub = 1; sub < mult; ++sub) {
                const auto read = input_reads(path, sub);
                packs |= unsigned(read.pack) << (sub - 1);
                resolves |= unsigned(read.resolve) << (sub - 1);
            }
            constexpr unsigned reconstructReads[]{1, 3, 7};
            const bool reconstruct = path == ShaderPath::Reconstruct;
            std::printf("OWNERSHIP %ux %s pack_mask=%u resolve_mask=%u\n",
                mult, reconstruct ? "reconstruct" : "raw", packs, resolves);
            test.expect(packs == 1 && resolves == (reconstruct ? reconstructReads[mult - 2] : 0u),
                "A/B recording and IPC fences share the same source-use policy");
        }
    }

    struct Scenario { bool active; ExitReason first; DWORD wait; HRESULT removed; HRESULT expected; };
    constexpr Scenario scenarios[]{
        {true, ExitReason::UserClosed, WAIT_TIMEOUT, S_OK, HRESULT_FROM_WIN32(ERROR_TIMEOUT)},
        {false, ExitReason::UserClosed, WAIT_TIMEOUT, S_OK, HRESULT_FROM_WIN32(ERROR_TIMEOUT)},
        {true, ExitReason::None, WAIT_TIMEOUT, S_OK, HRESULT_FROM_WIN32(ERROR_TIMEOUT)},
        {true, ExitReason::Watchdog, WAIT_TIMEOUT, DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_REMOVED},
    };
    unsigned index{};
    for (const auto& item : scenarios) {
        const auto dir = root / std::to_wstring(++index);
        StartingMarker marker(dir);
        const auto starting = dir / L"pushflow-starting.txt";
        const auto quarantine = dir / L"pushflow-crash-quarantine.txt";
        test.expect(marker.begin() == StartingMarker::Open::Armed && std::filesystem::exists(starting),
            "ring failure fixture starts with an armed startup marker");
        std::ofstream(quarantine) << "controller record\n";
        Session session{};
        stamp_exit_reason(session, item.first);
        bool shutdownSawFault{}, cleanup{}, propagated{};
        try {
            run_generation([&] { check_ring_wait(item.wait, item.removed); }, [&](bool fault) {
                shutdownSawFault = fault;
                stamp_exit_reason(session, ExitReason::Fault);
                const auto reason = read_exit_reason(session);
                cleanup = !item.active || reason != ExitReason::UserClosed || StartingMarker(dir).clear();
                return reason == ExitReason::UserClosed;
            });
        } catch (const ipc::Error& failure) {
            propagated = failure.code() == item.expected;
        }
        const bool closed = item.first == ExitReason::UserClosed;
        const auto reason = item.first == ExitReason::None ? ExitReason::Fault : item.first;
        test.expect(shutdownSawFault && cleanup && propagated != closed && read_exit_reason(session) == reason &&
            std::filesystem::exists(quarantine) && std::filesystem::exists(starting) != (closed && item.active),
            "ring failure honors first exit reason and preserves quarantine");
    }
}
} // namespace nr::fg::qa
