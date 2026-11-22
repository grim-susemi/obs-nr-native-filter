// SPDX-License-Identifier: GPL-3.0-only
#include "../fg-presenter/pushflow_motion.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace veyra::pushflow::motion;
namespace {
unsigned failures{};
void expect(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
void tier_geometry()
{
    struct Resolution {
        uint32_t width, height;
        const char* name;
        uint32_t modelW[3], modelH[3], padW[3], padH[3];
    };
    constexpr Resolution resolutions[]{
        {1920, 1080, "1080p", {960, 1280, 1920}, {540, 720, 1080}, {960, 1280, 1920}, {576, 768, 1088}},
        {2560, 1440, "1440p", {960, 1280, 1920}, {540, 720, 1080}, {960, 1280, 1920}, {576, 768, 1088}},
        {3840, 2160, "4K", {960, 1280, 1920}, {540, 720, 1080}, {960, 1280, 1920}, {576, 768, 1088}},
        {2560, 1080, "2560x1080", {960, 1280, 1920}, {405, 540, 810}, {960, 1280, 1920}, {448, 576, 832}},
        {1280, 1024, "1280x1024", {640, 900, 1280}, {512, 720, 1024}, {640, 960, 1280}, {512, 768, 1024}},
        {1024, 768, "1024x768", {512, 960, 1024}, {384, 720, 768}, {512, 960, 1024}, {384, 768, 768}},
    };
    constexpr const char* tiers[]{"Fast", "Balanced", "Quality"};
    for (const Resolution& resolution : resolutions) {
        for (uint32_t tier = 0; tier < kTiers; ++tier) {
            const double scale = networkScale(tier, resolution.width, resolution.height);
            const Geometry geometry = makeGeometry({resolution.width, resolution.height, tier, 32});
            char label[96]{};
            std::snprintf(label, sizeof(label), "%s %s axis caps and 64-pixel facts-pad floor",
                resolution.name, tiers[tier]);
            expect(geometry.modelW == resolution.modelW[tier] && geometry.modelH == resolution.modelH[tier] &&
                geometry.padW == resolution.padW[tier] && geometry.padH == resolution.padH[tier] &&
                geometry.valid() && (tier != 0 || scale <= 0.5), label);
            std::printf("MEASURE %s %s scale=%.9f model=%ux%u pad=%ux%u\n",
                resolution.name, tiers[tier], scale, geometry.modelW, geometry.modelH,
                geometry.padW, geometry.padH);
        }
    }
    const Geometry largerFactsPad = makeGeometry({2560, 1080, 2, 128});
    expect(largerFactsPad.modelW == 1920 && largerFactsPad.modelH == 810 &&
        largerFactsPad.padW == 1920 && largerFactsPad.padH == 896 &&
        largerFactsPad.padW % 128 == 0 && largerFactsPad.padH % 128 == 0,
        "facts.pad above 64 sets the product geometry padding multiple");
    const Geometry fastOverride = makeGeometry({3840, 2160, 0, 32, 1080});
    expect(fastOverride.modelW == 960 && fastOverride.modelH == 540,
        "Fast motion-height override remains inside both tier caps and half scale");
}
void coordinates()
{
    bool roundTrip = true;
    for (const bool alignCorners : {true, false}) {
        for (const uint32_t pad : {1u, 2u, 31u, 64u, 257u}) {
            if (pad == 1 && alignCorners) continue;
            for (const float x : {0.0f, 0.5f, 1.0f, 7.25f, static_cast<float>(pad - 1)}) {
                const float restored = gridToModel(modelToGrid(x, pad, alignCorners), pad, alignCorners);
                roundTrip = roundTrip && std::abs(restored - x) < 1e-4f;
            }
        }
    }
    expect(roundTrip, "grid and padded-model coordinates round-trip for both corner conventions");
    expect(std::abs(modelCoord(0, 3840, 960) + 0.375f) < 1e-6f &&
        std::abs(modelToSource(modelCoord(1234, 3840, 960), 3840, 960) - 1234.0f) < 1e-3f,
        "full-resolution coordinates sample the reduced motion field at pixel centers");
    expect(!Geometry{}.valid() && Geometry{256, 160, 128, 80, 128, 128}.valid() &&
        !Geometry{256, 160, 128, 80, 64, 128}.valid(),
        "geometry requires positive sizes and padding at least as large as the model");
}
void encoded_clamping()
{
    const Geometry geometry{1, 1, 1, 1, 64, 64};
    const auto color = reference::rebuild(geometry, 0, 0,
        [](uint32_t, uint32_t) { return reference::Rgb{-0.25f, 0.5f, 1.25f}; },
        [](uint32_t, uint32_t) { return reference::Rgb{0.25f, 0.5f, 0.75f}; },
        [](uint32_t, uint32_t) { return reference::Vec2{-1.0f, -1.0f}; },
        [](uint32_t, uint32_t) { return reference::Vec2{-1.0f, -1.0f}; },
        [](uint32_t, uint32_t) { return 1.0f; });
    expect(color.r == 0.0f && color.g == 0.5f && color.b == 1.0f,
        "normalized encoded samples clamp to [0, 1] during motion reconstruction");
}
}
int main()
{
    tier_geometry();
    coordinates();
    encoded_clamping();
    std::printf("%s pushflow_motion (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

