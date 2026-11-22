// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_vfg_session.h"
#include "vfg_content.h"
#include "vfg_shot_change.h"
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
    uint64_t compareFence_{};
    std::unique_ptr<VfgShotChange> shotChange_;
    uint32_t compareParity_{};
    std::array<uint64_t, 2> shotSlotFence_{};
    void init_compare();
    ID3D12GraphicsCommandList* begin();
    ID3D12GraphicsCommandList* begin_available();
    uint64_t finish(bool sync);
    using Draw = std::function<void(ID3D12GraphicsCommandList*)>;
    UINT present_ready(ID3D12Resource*, bool synthetic, int beforeIndex, uint32_t splitPercent);
public:
    struct SyntheticFrame {
        ID3D12Resource* texture;
        Draw resolve;
        unsigned prevIndex, currIndex;
        bool shotCompared;
        uint32_t shotParity;
    };
    struct CompareResult {
        bool same, cut, flash;
        uint32_t parity;
    };
    ComPtr<ID3D12CommandQueue> ipcQueue, drawQueue;
    std::array<ComPtr<ID3D12Resource>, content_input_count> input;
    std::array<ComPtr<ID3D12Resource>, content_input_count> before;
    std::array<ComPtr<ID3D12Resource>, max_multiplier - 1> syntheticTex;
    ID3D12Device* d3d_device() const noexcept { return device_.Get(); }
    [[nodiscard]] const PushFlowShaderBytecodeSet& pushflow_shader_bytecode() const noexcept
    {
        return pushflowShaders_;
    }
    Gpu(const Session&, HWND, uint32_t multiplier);
    void enter_steady_state() noexcept { waitMs_ = 1000; }
    // Call before submitting a copy to this input on ipcQueue.
    void prepare_input(unsigned index);
    [[nodiscard]] bool commands_ready() const;
    // The caller queues the exact IPC copy dependency before comparison and
    // retains both inputs until the result arrives. No CPU fence wait or Map
    // occurs on submission, and polling never maps an unfinished readback.
    [[nodiscard]] bool begin_compare(unsigned previous, unsigned current, HANDLE ready);
    [[nodiscard]] std::optional<CompareResult> compare_result();
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
    void set_hud(uint32_t pos, uint32_t inset, uint32_t w, uint32_t h, const uint32_t* pixels);
    UINT present(ID3D12Resource*, bool synthetic, int beforeIndex = -1, uint32_t splitPercent = 50);
    // Deadline is on clock_ns(). No GPU work is submitted on a missed admission.
    [[nodiscard]] std::optional<UINT> try_present_synthetic(const SyntheticFrame&, uint64_t deadlineNs,
        int beforeIndex = -1, uint32_t splitPercent = 50);
};
} // namespace nr::fg::vfg
