// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_vfg_session.h"
#include <wrl/client.h>
#include <array>

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
    uint64_t fence_{};
    bool allowTearing_{};
    D3D12_RESOURCE_DESC desc_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    ComPtr<ID3D12Resource> hudUpload_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT hudFp_{};
    uint32_t hudW_{}, hudH_{}, hudPos_{};
    ID3D12GraphicsCommandList* begin();
    void finish(bool sync);
    void move(ID3D12Resource*, ID3D12Resource*, bool toBuffer);
public:
    static constexpr size_t max_exchange = 2 + (max_multiplier - 1); // 2 inputs + up to 3 outputs (4x)
    ComPtr<ID3D12CommandQueue> ipcQueue, drawQueue;
    std::array<ComPtr<ID3D12Resource>, 2> input;
    std::array<ComPtr<ID3D12Resource>, max_exchange> exchange;
    std::array<Handle, max_exchange> exported;
    size_t outputCount{};
    uint64_t bytes{}, allocated{};
    UINT pitch() const { return footprint_.Footprint.RowPitch; }
    ID3D12Device* d3d_device() const noexcept { return device_.Get(); }
    Gpu(const Session&, HWND, uint32_t multiplier);
    void stage(unsigned previous, unsigned current);
    void set_hud(uint32_t pos, uint32_t w, uint32_t h, const uint32_t* pixels);
    UINT present(ID3D12Resource*, bool synthetic);
};
} // namespace nr::fg::vfg
