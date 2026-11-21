// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_vfg_session.h"
#include "vfg_content.h"
#include <wrl/client.h>
#include <array>
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
class Gpu final {
    HWND window_;
    ComPtr<ID3D12Device> device_;
    struct CmdSlot {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 fenceVal{};
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
    D3D12_RESOURCE_DESC desc_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    ComPtr<ID3D12Resource> hudUpload_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT hudFp_{};
    uint32_t hudW_{}, hudH_{}, hudPos_{}, hudInset_{};
    std::array<uint64_t, content_input_count> inputReadFence_{}, inputWaitFence_{};
    ComPtr<ID3D12RootSignature> compareRoot_;
    ComPtr<ID3D12PipelineState> comparePipeline_;
    ComPtr<ID3D12DescriptorHeap> compareViews_;
    UINT compareViewStride_{};
    ComPtr<ID3D12Resource> compareFlags_, compareZero_, compareReadback_;
    uint64_t compareFence_{};
    void init_compare();
    ID3D12GraphicsCommandList* begin();
    void finish(bool sync);
    void move(ID3D12Resource*, ID3D12Resource*, bool toBuffer);
    UINT present_ready(ID3D12Resource*, bool synthetic);
public:
    static constexpr size_t max_exchange = 2 + (max_multiplier - 1); // 2 inputs + up to 3 outputs (4x)
    ComPtr<ID3D12CommandQueue> ipcQueue, drawQueue;
    std::array<ComPtr<ID3D12Resource>, content_input_count> input;
    std::array<ComPtr<ID3D12Resource>, max_exchange> exchange;
    std::array<Handle, max_exchange> exported;
    size_t outputCount{};
    uint64_t bytes{}, allocated{};
    UINT pitch() const { return footprint_.Footprint.RowPitch; }
    ID3D12Device* d3d_device() const noexcept { return device_.Get(); }
    ID3D12Fence* staging_fence() const noexcept { return done_.Get(); }
    Gpu(const Session&, HWND, uint32_t multiplier);
    // Call before submitting a copy to this input on ipcQueue.
    void prepare_input(unsigned index);
    [[nodiscard]] bool commands_ready() const;
    // The caller queues the exact IPC copy dependency before comparison and
    // retains both inputs until the result arrives. No CPU fence wait or Map
    // occurs on submission, and polling never maps an unfinished readback.
    [[nodiscard]] bool begin_compare(unsigned previous, unsigned current, HANDLE ready);
    [[nodiscard]] std::optional<bool> compare_result();
    // IPC copies must be ordered before this read, and the previous CUDA pair
    // must be finished. An empty result means no staging commands were queued.
    [[nodiscard]] std::optional<uint64_t> try_stage(unsigned previous, unsigned current);
    void set_hud(uint32_t pos, uint32_t inset, uint32_t w, uint32_t h, const uint32_t* pixels);
    UINT present(ID3D12Resource*, bool synthetic);
    // Deadline is on clock_ns(). No GPU work is submitted on a missed admission.
    [[nodiscard]] std::optional<UINT> try_present_synthetic(ID3D12Resource*, uint64_t deadlineNs);
};
} // namespace nr::fg::vfg
