// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace nr::hud {
// Crisp 5x7 pixel font (LSB = left pixel, 5 bits wide, 7 rows tall)
inline std::array<uint8_t, 7> glyph(char ch) noexcept {
    switch (ch) {
    case '0': return {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
    case '1': return {0x04,0x06,0x04,0x04,0x04,0x04,0x0E};
    case '2': return {0x0E,0x11,0x10,0x0C,0x02,0x01,0x1F};
    case '3': return {0x0E,0x11,0x10,0x0C,0x10,0x11,0x0E};
    case '4': return {0x08,0x0C,0x0A,0x09,0x1F,0x08,0x08};
    case '5': return {0x1F,0x01,0x0F,0x10,0x10,0x11,0x0E};
    case '6': return {0x0C,0x02,0x01,0x0F,0x11,0x11,0x0E};
    case '7': return {0x1F,0x10,0x08,0x04,0x02,0x02,0x02};
    case '8': return {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    case '9': return {0x0E,0x11,0x11,0x1E,0x10,0x08,0x06};
    case 'F': return {0x1F,0x01,0x01,0x0F,0x01,0x01,0x01};
    case 'G': return {0x0E,0x11,0x01,0x1D,0x11,0x11,0x0E};
    case 'I': return {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
    case 'N': return {0x11,0x13,0x15,0x19,0x11,0x11,0x11};
    case 'O': return {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    case 'P': return {0x0F,0x11,0x11,0x0F,0x01,0x01,0x01};
    case 'R': return {0x0F,0x11,0x11,0x0F,0x05,0x09,0x11};
    case 'S': return {0x0E,0x11,0x01,0x0E,0x10,0x11,0x0E};
    case 'V': return {0x11,0x11,0x11,0x11,0x11,0x0A,0x04};
    case 'X': case 'x': return {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
    case 'f': return {0x0C,0x02,0x02,0x0F,0x02,0x02,0x02};
    case 'p': return {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01};
    case 's': return {0x00,0x00,0x0E,0x01,0x0E,0x10,0x0F};
    case '.': return {0x00,0x00,0x00,0x00,0x00,0x06,0x06};
    case ':': return {0x00,0x06,0x06,0x00,0x06,0x06,0x00};
    case '|': return {0x04,0x04,0x04,0x04,0x04,0x04,0x04};
    case '-': return {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    case '>': return {0x02,0x04,0x08,0x10,0x08,0x04,0x02};
    case '(': return {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
    case ')': return {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
    case '/': return {0x10,0x10,0x08,0x04,0x02,0x01,0x01};
    default:  return {0,0,0,0,0,0,0};
    }
}

inline constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

struct HudImage {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint32_t> pixels;
};

// Renders a crisp dark pill HUD: "<leftText>  ->  <rightText>"
// bgAlpha = 255 for opaque copy onto D3D12 backbuffer, or ~215 for OBS alpha blend.
inline HudImage render_fps_pill(std::string_view leftText, std::string_view arrowText,
    std::string_view rightText, uint32_t scale, uint8_t bgAlpha,
    uint32_t rightColor = rgba(0x3D, 0xDC, 0x84, 0xFF)) {
    scale = std::clamp<uint32_t>(scale, 1u, 4u);
    const uint32_t charAdvance = 6u * scale;
    const uint32_t padX = 10u * scale;
    const uint32_t padY = 6u * scale;
    const size_t totalChars = leftText.size() + arrowText.size() + rightText.size();
    const uint32_t w = std::max<uint32_t>(64u, padX * 2u + static_cast<uint32_t>(totalChars) * charAdvance - scale);
    const uint32_t h = padY * 2u + 7u * scale;

    HudImage img{w, h, std::vector<uint32_t>(size_t(w) * h, rgba(14, 18, 26, bgAlpha))};
    const uint32_t border = rgba(62, 78, 102, bgAlpha);
    for (uint32_t x = 0; x < w; ++x) {
        img.pixels[x] = border;
        img.pixels[size_t(h - 1) * w + x] = border;
    }
    for (uint32_t y = 0; y < h; ++y) {
        img.pixels[size_t(y) * w] = border;
        img.pixels[size_t(y) * w + (w - 1)] = border;
    }
    // Cut 2px corners slightly softer
    const uint32_t corner = bgAlpha == 255 ? rgba(20, 26, 36, 255) : rgba(0, 0, 0, 0);
    if (w > 4 && h > 4) {
        img.pixels[0] = img.pixels[w - 1] = img.pixels[size_t(h - 1) * w] = img.pixels[size_t(h) * w - 1] = corner;
    }

    auto drawSpan = [&](std::string_view text, uint32_t startChar, uint32_t color) {
        for (size_t idx = 0; idx < text.size(); ++idx) {
            const auto g = glyph(text[idx]);
            const uint32_t ox = padX + static_cast<uint32_t>(startChar + idx) * charAdvance;
            for (uint32_t row = 0; row < 7; ++row) {
                for (uint32_t col = 0; col < 5; ++col) {
                    if ((g[row] >> col) & 1u) {
                        for (uint32_t sy = 0; sy < scale; ++sy) {
                            for (uint32_t sx = 0; sx < scale; ++sx) {
                                const uint32_t px = ox + col * scale + sx;
                                const uint32_t py = padY + row * scale + sy;
                                if (px < w && py < h) img.pixels[size_t(py) * w + px] = color;
                            }
                        }
                    }
                }
            }
        }
    };

    const uint32_t cLeft = rgba(0xE8, 0xEE, 0xF7, 0xFF);
    const uint32_t cArrow = rgba(0x88, 0x99, 0xB2, 0xFF);
    drawSpan(leftText, 0, cLeft);
    drawSpan(arrowText, static_cast<uint32_t>(leftText.size()), cArrow);
    drawSpan(rightText, static_cast<uint32_t>(leftText.size() + arrowText.size()), rightColor);
    return img;
}
}
