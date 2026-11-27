// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_backend.h"
#include "fg_vfg_session.h"
#include "vfg_content.h"
#include "vfg_shot_change.h"
#include "vfg_hud_lock.h"
#include <wrl/client.h>
#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace nr::fg::vfg {
using Microsoft::WRL::ComPtr;
struct Handle {
    HANDLE h{};
    ~Handle() { if (h) CloseHandle(h); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
inline void check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) throw ipc::Error(ipc::Status::ProtocolError, hr, operation);
}
inline void check_ring_wait(DWORD wait, HRESULT removed)
{
    check(removed, "present device removed");
    if (wait != WAIT_OBJECT_0) check(
        wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_FAIL, "present ring wait");
}
struct PushFlowShaderBytecodeSet {
    D3D12_SHADER_BYTECODE pack{}, unpack{}, reconstruct{};
};
[[nodiscard]] PushFlowShaderBytecodeSet make_pushflow_shader_bytecode() noexcept;
class DescriptorStager final {
    ID3D12Device* device_;
    ComPtr<ID3D12DescriptorHeap> cpu_;
public:
    explicit DescriptorStager(ID3D12Device*);
    void write_srv(ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE dest) const;
    void write_uav(ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE dest) const;
};

class Gpu final {
    HWND window_;
    bool pushflow_;
    const PushFlowShaderBytecodeSet pushflowShaders_{make_pushflow_shader_bytecode()};
    ComPtr<ID3D12Device> device_;
    struct CmdSlot {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 fenceVal{};
        std::shared_ptr<VfgHudLock> hudUse;
    };
    static constexpr size_t ring_slots = 4;
    std::array<CmdSlot, ring_slots> ring_{};
    size_t ringIdx_{};
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Fence> done_;
    Handle event_;
    Handle latency_;
    bool latencyReady_{};
    uint64_t fence_{};
    bool allowTearing_{};
    DWORD waitMs_{5000};
    D3D12_RESOURCE_DESC desc_{};
    ComPtr<ID3D12Resource> hudUpload_;
    ComPtr<ID3D12Resource> dividerUpload_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT dividerFp_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT hudFp_{};
    uint32_t hudW_{}, hudH_{}, hudPos_{}, hudInset_{};
    std::array<uint64_t, content_input_count> inputReadFence_{}, inputWaitFence_{};
    ComPtr<ID3D12RootSignature> compareRoot_;
    ComPtr<ID3D12PipelineState> comparePipeline_;
    ComPtr<ID3D12DescriptorHeap> compareViews_;
    UINT compareViewStride_{};
    ComPtr<ID3D12Resource> compareFlags_, compareZero_, compareReadback_;
    uint64_t compareFence_{}, discardedCompareFence_{};
    std::unique_ptr<VfgShotChange> shotChange_;
    std::shared_ptr<VfgHudLock> hudLock_;
    uint32_t compareParity_{};
    uint32_t compareDedupMode_{};
    std::array<uint64_t, VfgShotChange::slot_count> shotSlotFence_{};
    void init_compare();
    ID3D12GraphicsCommandList* begin();
    ID3D12GraphicsCommandList* begin_available();
    uint64_t finish(bool sync);
    using Draw = std::function<void(ID3D12GraphicsCommandList*)>;
    UINT present_ready(ID3D12Resource*, bool synthetic, int beforeIndex, uint32_t splitPercent, bool splitSwap);
public:
    struct SyntheticFrame {
        ID3D12Resource* texture;
        Draw resolve;
        unsigned prevIndex, currIndex;
        bool shotCompared;
        uint32_t shotParity;
        bool hudLock;
        unsigned subframe;
        unsigned slotParity = 0;
    };
    struct CompareResult {
        bool same, cut, flash;
        uint32_t parity;
    };
    struct PresentCopy {
        ID3D12Resource* source;
        ID3D12Resource* destination;
        int beforeIndex = -1;
        uint32_t splitPercent = 50;
        bool splitSwap = false;
    };
    ComPtr<ID3D12CommandQueue> ipcQueue, drawQueue;
    std::array<ComPtr<ID3D12Resource>, content_input_count> input;
    std::array<ComPtr<ID3D12Resource>, content_input_count> before;
    std::array<ComPtr<ID3D12Resource>, 2 * (max_multiplier - 1)> syntheticTex;
    ID3D12Device* d3d_device() const noexcept { return device_.Get(); }
    [[nodiscard]] const PushFlowShaderBytecodeSet& pushflow_shader_bytecode() const noexcept
    {
        return pushflowShaders_;
    }
    Gpu(const Session&, HWND, uint32_t multiplier);
    void enter_steady_state() noexcept { waitMs_ = 1000; }
    // Called before submitting a copy to this input on ipcQueue.
    void prepare_input(unsigned index);
    // Insert a draw-queue point after the caller's IPC copy ordering and return
    // it. Imported motion consumers wait on this instead of assuming CPU arrival
    // proves GPU completion.
    FgFencePoint signal_flow_ready();
    [[nodiscard]] bool commands_ready() const;
    // The caller queues the exact IPC copy dependency before comparison and
    // retains both inputs until the result arrives. No CPU fence wait or Map
    // occurs on submission, and polling never maps an unfinished readback.
    // dedupMode 0 compares bytes exactly; any other value tolerates
    // content_band_steps LSB per RGB component up to content_over_budget().
    // shotParity selects the VfgShotChange bank; UINT32_MAX records into the
    // transient slot so repeat frames never clobber an active pair.
    [[nodiscard]] bool begin_compare(unsigned previous, unsigned current, HANDLE ready,
        uint32_t dedupMode = 0, uint32_t shotParity = UINT32_MAX);
    [[nodiscard]] std::optional<CompareResult> compare_result();
    // Drop the result now, but keep its readback unavailable until GPU completion.
    void discard_compare() noexcept;
    // Speculative admission never waits on the ring. Zero means no submission.
    [[nodiscard]] uint64_t record_draw(const Draw&);
    void mark_inputs_read(unsigned prevIndex, unsigned currIndex, uint64_t fenceVal);
    // A backend may own its command allocators but still reads these inputs
    // on drawQueue. Insert its reader fence into the IPC recycle timeline.
    void mark_external_inputs_read(unsigned prevIndex, unsigned currIndex)
    {
        const auto value = ++fence_;
        check(drawQueue->Signal(done_.Get(), value), "backend reader fence");
        mark_inputs_read(prevIndex, currIndex, value);
    }
    // Called at the presenter poll, even without an incoming original.
    void sync_hud_lock(bool enabled);
    // On exceptional unwind only; a confirmed lost device needs no live-queue drain.
    [[nodiscard]] bool drain_hud_on_unwind() const noexcept;
    void update_hud_lock(unsigned current, unsigned previous, bool reset);
    [[nodiscard]] std::weak_ptr<const VfgHudLock> hud_lock() const noexcept { return hudLock_; }
    void set_hud(uint32_t pos, uint32_t inset, uint32_t w, uint32_t h, const uint32_t* pixels);
    // Synthetic display must be recorded on the current Gpu ring command list.
    [[nodiscard]] ID3D12Resource* record_synthetic_display(ID3D12GraphicsCommandList*, const SyntheticFrame&);
    void record_present_copy(ID3D12GraphicsCommandList*, const PresentCopy&);
    UINT present(ID3D12Resource*, bool synthetic, int beforeIndex = -1, uint32_t splitPercent = 50, bool splitSwap = false);
    // Deadline is on clock_ns(). No GPU work is submitted on a missed admission.
    [[nodiscard]] std::optional<UINT> try_present_synthetic(const SyntheticFrame&, uint64_t deadlineNs,
        int beforeIndex = -1, uint32_t splitPercent = 50, bool splitSwap = false);
};
} // namespace nr::fg::vfg
