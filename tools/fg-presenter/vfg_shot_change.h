// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace nr::fg::vfg {
class VfgShotChange final {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> results_;
    Microsoft::WRL::ComPtr<ID3D12Resource> zero_;
    bool initialized_{};
public:
    // Every parity slot owns one 256-byte window of the results buffer, so
    // four pair banks and the transient unpinned compare never share a
    // SetPredication offset.
    static constexpr uint32_t pair_slots = 4;
    static constexpr uint32_t transient_slot = pair_slots;
    static constexpr uint32_t slot_count = pair_slots + 1;
    static constexpr uint32_t slot_bytes = 256;
    static constexpr uint32_t cut_word(uint32_t slot) { return slot * (slot_bytes / sizeof(uint32_t)); }
    static constexpr uint32_t flash_word(uint32_t slot) { return cut_word(slot) + 7; }
    static constexpr uint32_t predicate_offset(uint32_t slot) { return slot * slot_bytes; }
    // The shader keeps its two-word predicate and its diagnostic vector at the
    // head of whichever window it is dispatched against.
    struct Pair {
        D3D12_GPU_DESCRIPTOR_HANDLE previous, current;
        uint32_t width, height, parity, compare;
    };
    explicit VfgShotChange(ID3D12Device*);
    // readback is laid out as the 256-byte content-compare flags followed by
    // slot_count result windows, one per parity.
    void record(ID3D12GraphicsCommandList*, const Pair&, ID3D12Resource* readback);
    void replace_on_cut(ID3D12GraphicsCommandList*, ID3D12Resource* display,
        ID3D12Resource* current, uint32_t parity) const;
};
} // namespace nr::fg::vfg
