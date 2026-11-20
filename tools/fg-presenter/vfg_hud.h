// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_fps_hud.h"
#include <string>

namespace nr::fg::vfg::hud {
// Units are pixels in the existing 5x7 font's grid. Both ink and the
// backplate are opaque: CopyTextureRegion copies pixels rather than blending.
inline constexpr uint32_t ink = nr::hud::rgba(232, 238, 247, 255);
inline constexpr uint32_t plate = nr::hud::rgba(20, 26, 36, 255);
inline constexpr uint32_t inset = 4, padX = 3, padY = 2;
inline constexpr uint32_t glyphW = 5, glyphH = 7, advance = glyphW + 1;

inline uint32_t scale_for(uint32_t width, uint32_t height, uint32_t clientW, uint32_t clientH, uint32_t dpi)
{
    // The swap chain stretches the source to the window. Aim for a 14-DIP
    // cap height on the display, but limit it to 7 source pixels per 180 rows.
    const double ratio = std::min(double(width) / clientW, double(height) / clientH);
    const auto desired = static_cast<uint32_t>(2.0 * (dpi ? dpi : 96u) / 96.0 * ratio + 0.5);
    const auto limit = std::clamp(std::min(width / 320u, height / 180u), 1u, 3u);
    return std::clamp(desired, 1u, limit);
}

inline std::string label(uint32_t input, uint32_t presented)
{
    // '>' is drawn as a single right arrow, not two full-width ASCII cells.
    return (presented > input ? std::to_string(input) + ">" : "") + std::to_string(presented) + " FPS";
}

inline nr::hud::HudImage raster(std::string_view text, uint32_t scale)
{
    const uint32_t w = (2 * padX + static_cast<uint32_t>(text.size()) * advance - 1) * scale;
    const uint32_t h = (2 * padY + glyphH) * scale;
    nr::hud::HudImage img{w, h, std::vector<uint32_t>(size_t(w) * h, plate)};
    uint32_t x = padX * scale;
    for (char c : text) {
        auto glyph = nr::hud::glyph(c);
        if (c == '>') glyph = {0, 4, 8, 31, 8, 4, 0};
        for (uint32_t row = 0; row < glyphH; ++row) {
            for (uint32_t col = 0; col < glyphW; ++col) {
                if (!(glyph[row] & (1u << col))) continue;
                for (uint32_t dy = 0; dy < scale; ++dy) {
                    auto* dst = img.pixels.data() + size_t((padY + row) * scale + dy) * w + x + col * scale;
                    std::fill_n(dst, scale, ink);
                }
            }
        }
        x += advance * scale;
    }
    return img;
}
} // namespace nr::fg::vfg::hud
