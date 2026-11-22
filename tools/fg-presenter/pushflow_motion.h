// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): This work is based on PushFPS, licensed under GPL-3.0-or-later; port modified 2026-11-21.
#pragma once
//
// PushFlow's motion contract: what the tensors a model gives besides its picture mean, how positions
// convert between the motion field and the picture, and a reference of the full-size rebuild. Pure (no
// graphics, builds anywhere, unit-tested in tests/unit/PushFlowMotionTests.cpp); `reference::rebuild`
// is what shaders/PushFlowReconstruct.hlsl is compared against on WARP (PushGpuTests.cpp,
// testPushFlow): change both together.
//
// THE CONTRACT (docs/PUSHFLOW.md has the same in words)
//
// Besides its picture, a PushFlow model gives three tensors for the in-between frame at time t
// (scripts/pushflow/rife_graph.py exposes them and checks that they reproduce the model's picture):
//
//   grid0, grid1  [padH, padW, 2] floats, x then y. For every pixel of the in-between frame: WHERE TO
//                 READ the previous real frame (grid0) and the current real frame (grid1). GridSample
//                 convention: -1..1 across the padded model picture, `align_corners` as the model
//                 says (<model>.onnx.txt). They are backward sampling positions for this one time t.
//   mask          [padH, padW] 0..1: the share of the previous-frame sample in the blend (of the
//                 current-frame sample when the model's `mask_for_first` is 0).
//
//   picture = sample(previous, grid0) * w + sample(current, grid1) * (1 - w),  w = that share.
//
// What they are NOT:
//  - not optical flow between the two real frames. The network works each time t out on its own,
//    occlusion included; dividing a grid's displacement by t does not give the motion from one real
//    frame to the next, and nothing here pretends it does.
//  - the mask is a blend weight, not a confidence and not a probability of error. Measured on the
//    shipped models (scripts/pushflow/prepare_runtime.py motion_report; pushflow-models run
//    44595965906, 19 November 2026): an object that moves further than the network can follow is left
//    out of the in-between frame. At its old place the mask takes the current frame, at its new place
//    the previous one, and each shows background there. The mask is as decided there as anywhere, and
//    where it does keep both samples they agree. A mask that keeps both samples where they differ is
//    what a fade looks like, not what a failure looks like: a guard built on "the mask is undecided"
//    was written, measured and taken out again (docs/PUSHFLOW2.md).
//
// Sizes: the model runs on modelW x modelH (the tier's share of the picture), padded by repeating the
// edge to padW x padH (a multiple of the model's block). Pixel centres are at whole numbers. An output
// pixel p reads grids and mask at  (p + 0.5) * model / src - 0.5  (bilinear, clamped to the model
// area), turns the grid value into a position in the padded model picture, and that into a position in
// the full-size real frame:  (xm + 0.5) * src / model - 0.5  (bilinear, clamped to the frame). So the
// motion is enlarged, never the picture.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace veyra::pushflow::motion {

// --- the size the motion is worked out at --------------------------------------------------------
// Each tier has a model and a largest motion size: 0 Fast, the lite model at up to 960 x 540 and at
// most half the picture; 1 Balanced, the lite model at up to 1280 x 720; 2 Quality, the full model at
// up to 1920 x 1080. Never the picture's own size above that: at 4K every tier was too slow for live
// play (field report, 17 November 2026).
//
// Since 3.2.7 the motion size can be chosen apart from the tier (preference pushFlowMotion, Professional
// mode): 540, 720 or 1080 lines, i.e. at most 960 x 540, 1280 x 720 or 1920 x 1080 picture elements,
// whatever the tier's model. 0 keeps the tier's own size, which is what every user had before. The
// picture is rebuilt at full size from the full-size real frames either way. Why offer it: the shipped
// models do not behave the same at every size (docs/PUSHFLOW2.md: the full model followed some fast
// motion further at half size and lost fine motion there), and which size suits a game can only be
// found by measuring that game (the frame quality test).
inline constexpr uint32_t kTiers = 3;
inline constexpr int kMotionHeights[] = {0, 540, 720, 1080};
inline constexpr bool motionHeightValid(int lines) { return lines == 0 || lines == 540 || lines == 720 || lines == 1080; }
// The network's share of the picture's width and height.
inline double networkScale(uint32_t tier, uint32_t width, uint32_t height, int motionHeight = 0) {
    if (!width || !height) return 0.0;
    static constexpr double kMaxWidth[kTiers] = {960, 1280, 1920};
    static constexpr double kMaxHeight[kTiers] = {540, 720, 1080};
    const uint32_t index = std::min(tier, kTiers - 1);
    double capWidth = kMaxWidth[index];
    double capHeight = kMaxHeight[index];
    if (motionHeightValid(motionHeight) && motionHeight > 0) {
        capWidth = std::min(capWidth, double(motionHeight) * 16.0 / 9.0);
        capHeight = std::min(capHeight, double(motionHeight));
    }
    return std::min({tier == 0 ? 0.5 : 1.0, capWidth / double(width), capHeight / double(height)});
}
// Process-wide, set by the interface, read when the graph is described: a change rebuilds the graph
// (EngineController compares EnhanceGraphDesc::pushFlowMotion). Outside EnhancementSettings, so the
// preset schema shared with Veyra is unchanged.
namespace detail {
inline std::atomic<int>& motionHeightStore() {
    static std::atomic<int> lines{0};
    return lines;
}
}
inline void setMotionHeight(int lines) { detail::motionHeightStore().store(motionHeightValid(lines) ? lines : 0, std::memory_order_relaxed); }
inline int motionHeight() { return detail::motionHeightStore().load(std::memory_order_relaxed); }

// What the too-slow watch does when the card cannot keep up (pushFlowBudgetWatch): the step that
// makes PushFlow cheaper, never one that changes nothing.
//  - no motion size chosen: one tier down, as before 3.2.7 (Quality > Balanced > Fast), nothing at Fast;
//  - a chosen size larger than the tier's own, or any chosen size at Fast (Fast's own is never the
//    larger one): back to the tier's own size, the tier stays;
//  - Quality with a chosen size no larger than its own: one tier down, the size stays (the lite model
//    at the same size is the cheaper one);
//  - Balanced with a chosen size no larger than its own: Fast uses the same model, so only the tier
//    and the size together make it cheaper.
enum class SlowStep { Nothing, TierDown, MotionToAuto, TierDownAndMotionToAuto };
inline SlowStep slowStep(uint32_t tier, int motionHeight) {
    tier = std::min(tier, kTiers - 1);
    if (!motionHeightValid(motionHeight) || motionHeight == 0) return tier > 0 ? SlowStep::TierDown : SlowStep::Nothing;
    const uint32_t chosen = motionHeight >= 1080 ? 2u : motionHeight >= 720 ? 1u : 0u;   // the tier whose own size this is
    if (tier == 0 || chosen > tier) return SlowStep::MotionToAuto;
    return tier == 1 ? SlowStep::TierDownAndMotionToAuto : SlowStep::TierDown;
}

// --- coordinates ---------------------------------------------------------------------------------
struct Geometry {
    uint32_t srcW = 0, srcH = 0;       // the real frames and the picture made
    uint32_t modelW = 0, modelH = 0;   // the part of the padded tensor that holds the picture
    uint32_t padW = 0, padH = 0;       // the tensors' size
    bool alignCorners = true;
    bool maskForFirst = true;
    bool valid() const { return srcW && srcH && modelW && modelH && padW >= modelW && padH >= modelH; }
};
struct GeometryRequest {
    uint32_t srcW{}, srcH{}, tier{}, factsPad{};
    int motionHeight{};
    bool alignCorners{true};
    bool maskForFirst{true};
};
inline Geometry makeGeometry(const GeometryRequest& request) {
    const double scale = networkScale(request.tier, request.srcW, request.srcH, request.motionHeight);
    const auto modelW = static_cast<uint32_t>(std::llround(double(request.srcW) * scale));
    const auto modelH = static_cast<uint32_t>(std::llround(double(request.srcH) * scale));
    const uint64_t padMultiple = std::max<uint64_t>(request.factsPad, 64);
    const auto padded = [padMultiple](uint32_t modelSize) {
        const uint64_t size = ((uint64_t{modelSize} + padMultiple - 1) / padMultiple) * padMultiple;
        return size <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(size) : 0u;
    };
    return {request.srcW, request.srcH, modelW, modelH, padded(modelW), padded(modelH),
        request.alignCorners, request.maskForFirst};
}
// An output pixel -> where grids and mask are read (continuous model-pixel position).
inline float modelCoord(float outPixel, uint32_t src, uint32_t model) { return (outPixel + 0.5f) * float(model) / float(src) - 0.5f; }
// A grid value (-1..1) -> a position in the padded model picture.
inline float gridToModel(float g, uint32_t pad, bool alignCorners) {
    return alignCorners ? (g + 1.0f) * 0.5f * (float(pad) - 1.0f) : ((g + 1.0f) * float(pad) - 1.0f) * 0.5f;
}
// The grid value that reads padded-model position x (the inverse of gridToModel).
inline float modelToGrid(float x, uint32_t pad, bool alignCorners) {
    if (alignCorners) return pad > 1 ? 2.0f * x / (float(pad) - 1.0f) - 1.0f : 0.0f;
    return (2.0f * x + 1.0f) / float(pad) - 1.0f;
}
// A model position -> a position in the full-size real frame.
inline float modelToSource(float xm, uint32_t src, uint32_t model) { return (xm + 0.5f) * float(src) / float(model) - 0.5f; }
inline float gridToSource(float g, uint32_t src, uint32_t model, uint32_t pad, bool alignCorners) {
    return modelToSource(gridToModel(g, pad, alignCorners), src, model);
}
// The grid value that reads full-size position x (tests and identity grids).
inline float sourceToGrid(float x, uint32_t src, uint32_t model, uint32_t pad, bool alignCorners) {
    return modelToGrid((x + 0.5f) * float(model) / float(src) - 0.5f, pad, alignCorners);
}

// --- reference (what the shader does, in the same order) -----------------------------------------
namespace reference {
struct Rgb { float r = 0, g = 0, b = 0; };
// Bilinear read of a field at a continuous position, clamped to [0, w-1] x [0, h-1].
// at(x, y) returns the value at a whole position.
template <typename T, typename At>
T bilinear(float qx, float qy, uint32_t w, uint32_t h, At&& at) {
    qx = std::clamp(qx, 0.0f, float(w - 1)); qy = std::clamp(qy, 0.0f, float(h - 1));
    const uint32_t ax = uint32_t(qx), ay = uint32_t(qy);
    const uint32_t bx = std::min(ax + 1, w - 1), by = std::min(ay + 1, h - 1);
    const float fx = qx - float(ax), fy = qy - float(ay);
    const T top = at(ax, ay) + (at(bx, ay) - at(ax, ay)) * fx;
    const T bottom = at(ax, by) + (at(bx, by) - at(ax, by)) * fx;
    return top + (bottom - top) * fy;
}
inline Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline Rgb operator-(Rgb a, Rgb b) { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
inline Rgb operator*(Rgb a, float s) { return {a.r * s, a.g * s, a.b * s}; }
struct Vec2 { float x = 0, y = 0; };
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }

// One pixel of the rebuilt picture, encoded 0..1. previous(x, y) / current(x, y): the real frames'
// encoded colours; grid0(x, y) / grid1(x, y): the grids at a whole padded-model position; mask(x, y):
// likewise.
template <typename FrameA, typename FrameB, typename Grid0, typename Grid1, typename Mask>
Rgb rebuild(const Geometry& g, uint32_t x, uint32_t y, FrameA&& previous, FrameB&& current, Grid0&& grid0, Grid1&& grid1, Mask&& mask) {
    const float qx = modelCoord(float(x), g.srcW, g.modelW), qy = modelCoord(float(y), g.srcH, g.modelH);
    const Vec2 g0 = bilinear<Vec2>(qx, qy, g.modelW, g.modelH, grid0), g1 = bilinear<Vec2>(qx, qy, g.modelW, g.modelH, grid1);
    const auto sample = [&](Vec2 grid, auto&& frame) {
        return bilinear<Rgb>(gridToSource(grid.x, g.srcW, g.modelW, g.padW, g.alignCorners), gridToSource(grid.y, g.srcH, g.modelH, g.padH, g.alignCorners),
                             g.srcW, g.srcH, frame);
    };
    const auto saturate = [](Rgb c) { return Rgb{std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f)}; };
    const Rgb a = saturate(sample(g0, previous)), b = saturate(sample(g1, current));
    const float m = std::clamp(bilinear<float>(qx, qy, g.modelW, g.modelH, mask), 0.0f, 1.0f);
    return g.maskForFirst ? a * m + b * (1.0f - m) : a * (1.0f - m) + b * m;
}
} // namespace reference

} // namespace veyra::pushflow::motion

