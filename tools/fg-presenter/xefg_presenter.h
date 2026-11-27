// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Official 2x XeFG presenter: the proxy swapchain owns the HWND, every original
// is tagged with 2D constants (neutral depth, identity view, fixed projection,
// zero jitter, shared-NVOF motion) and presented through the XeSS swap chain.
#include "fg_backend.h"
#include "vfg_gpu.h"
#include "xefg_runtime.h"
#include <wrl/client.h>
#include <memory>
#include <optional>

namespace nr::fg::vfg {
struct XeFgFrameInput {
    bool shotCut{};
    bool discontinuity{};
    bool flowReset{};
    float mvScaleX{1.0f}, mvScaleY{1.0f};
    float frameRenderTimeMs{0.0f};
    uint32_t width{1920}, height{1080};
};

// 2D adaptation of the official frame constants. Identity view, fixed 60 degree
// vertical projection, zero jitter, no history reset unless the caller asks.
// The projection is row-major like the XeSS structure.
xefg_swapchain_frame_constant_data_t xefg_frame_constants(const XeFgFrameInput& input) noexcept;

// Neutral depth (R32_FLOAT = 1.0) plus a zero motion fallback and two rotating
// motion proxies. XeFG reads the tagged resources on its own queue work, so a
// shared-NVOF texture is copied into a private proxy that the transport can
// recycle immediately. Extents follow the guide extent whenever shared motion is
// present and the render extent otherwise.
class XeFgGuides final {
    ComPtr<ID3D12Resource> depth_, zeroMotion_, motion_[2], depthUpload_, zeroUpload_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFp_{}, motionFp_{};
    uint32_t width_{}, height_{};
    unsigned slot_{};
    bool filled_{};
public:
    bool ensure(Gpu&, uint32_t width, uint32_t height);
    void reset() noexcept;
    [[nodiscard]] ID3D12Resource* depth() const noexcept { return depth_.Get(); }
    [[nodiscard]] ID3D12Resource* zero_motion() const noexcept { return zeroMotion_.Get(); }
    // Rotates a private MV proxy and returns it for tagging.
    [[nodiscard]] ID3D12Resource* take_motion_slot() noexcept;
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
#ifdef NR_FG_TEST_MOCK_XESS
    // Copies the neutral depth to a readback buffer on the supplied queue and
    // verifies every texel is 1.0. Bounded CPU wait, test-only.
    [[nodiscard]] bool verify_neutral(ID3D12Device*, ID3D12CommandQueue*, DWORD timeoutMs = 2000) const;
#endif
};

// One tagged input for a present. Grouping the API's record fields keeps the
// private helper at one argument plus the frame identity.
struct XeFgTagRequest {
    ID3D12Resource* resource{};
    xefg_swapchain_resource_type_t type{};
    xefg_swapchain_resource_validity_t validity{};
    D3D12_RESOURCE_STATES state{};
    uint32_t width{}, height{};
    const char* name{};
};

class XeFgPresenter final : public IFgBackend {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    XeFgPresenter(Gpu&, Session&, FILE* log);
    ~XeFgPresenter() override;
    XeFgPresenter(const XeFgPresenter&) = delete;
    XeFgPresenter& operator=(const XeFgPresenter&) = delete;
    // Takes ownership of the admitted runtime. False means every XeFG/XeLL
    // object created so far was released and the caller must not present.
    bool open(std::unique_ptr<XeFgRuntime>&& runtime);
    const char* name() const noexcept override { return "xefg"; }
    unsigned version_tag() const noexcept override;
    bool faulted() const noexcept override;
    std::optional<FgPairTicket> submit_pair(const FgPairDesc&, unsigned prevIndex, unsigned currIndex) override;
    bool subframe_ready(const FgPairTicket&, uint32_t sub) override;
    void resolve_subframe(ID3D12GraphicsCommandList*, const FgPairTicket&, ID3D12Resource*, uint32_t) override;
    size_t max_in_flight_pairs() const noexcept override { return 1; }
    void synchronize() override {}
    bool close() noexcept override;
    bool owns_presentation() const noexcept override { return true; }
    std::optional<UINT> present_original(const FgOriginalDesc&) override;
private:
    bool tag_resource(ID3D12GraphicsCommandList*, uint32_t presentId, const XeFgTagRequest&);
    [[nodiscard]] bool drain_draw_queue(DWORD timeoutMs);
    void log_present_status(uint32_t presentId);
};
} // namespace nr::fg::vfg
