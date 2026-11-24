// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "anr_guides.h"
#include "anr_runtime.h"
#include "vfg_gpu.h"

namespace nr::fg::vfg {
class AnrCommands {
    ID3D12CommandQueue* queue_;
    ComPtr<ID3D12CommandAllocator> alloc_;
    std::array<ComPtr<ID3D12GraphicsCommandList>, 1 + 2 * (max_multiplier - 1)> lists_;
    size_t cursor_{};
    ComPtr<ID3D12Fence> fence_;
    Handle event_;
    UINT64 last_{};
    bool untracked_{};
public:
    AnrCommands(ID3D12Device*, ID3D12CommandQueue*);
    ID3D12GraphicsCommandList* begin();
    NV_OF_FENCE_POINT submit(ID3D12GraphicsCommandList*);
    void execute(ID3D12GraphicsCommandList*);
    void enqueue_wait(NV_OF_FENCE_POINT);
    NV_OF_FENCE_POINT seal();
    void discard(ID3D12GraphicsCommandList*);
    void wait_for(NV_OF_FENCE_POINT);
    void wait();
    bool complete(UINT64) const;
    void recycle();
    NV_OF_FENCE_POINT last_point() const { return {fence_.Get(), last_}; }
};

struct AnrOutputSlot {
    ComPtr<ID3D12Resource> image, readback;
    UINT64 fence{};
};

class AnrDispatch {
    FILE* trace_;
    ComPtr<ID3D12Resource> disable_, initial_;
    std::array<AnrOutputSlot, max_multiplier - 1> slots_;
    bool restorePending_{};
public:
    struct Configuration { Extent size; unsigned generated; FILE* trace; };
    AnrDispatch(ID3D12Device*, Configuration);
    // The Vulkan producer must be submitted before recording any consumer commands.
    bool evaluate(AnrCommands&, AnrFeature&, AnrEvaluation);
    bool input_state_confirmed() const noexcept { return !restorePending_; }
    const AnrOutputSlot& slot(unsigned sub) const { return slots_[sub - 1]; }
};
} // namespace nr::fg::vfg
