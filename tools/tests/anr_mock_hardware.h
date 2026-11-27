// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "anr_mock_gpu.h"
#include "fg_interpolation_oracle.h"
#include <filesystem>

namespace anr_test {
using nr::fg::qa::Square;
constexpr Square left{96, 148, 28}, right{224, 148, 28};

template<class Pixel> std::vector<Pixel> square(Square rect, Pixel white, Pixel black)
{
    std::vector<Pixel> pixels(extent.width * extent.height, black);
    for (int y = rect.y; y < rect.y + rect.width; ++y) {
        const auto offset = static_cast<size_t>(y) * extent.width + static_cast<size_t>(rect.x);
        std::fill_n(pixels.begin() + static_cast<ptrdiff_t>(offset), rect.width, white);
    }
    return pixels;
}

inline void verify_oracle()
{
    using namespace nr::fg::qa;
    const auto a = square(left, 1.f, 0.f), b = square(right, 1.f, 0.f);
    const auto dims = [](std::span<const float> pixels) {
        return LuminanceFrame{static_cast<int>(extent.width), static_cast<int>(extent.height), pixels};
    };
    const auto at = [](double t) { return square(square_at(left, right, t), 1.f, 0.f); };
    const auto f1 = at(0.25), f2 = at(0.5), f3 = at(0.75);
    const InterpolatedFrame exact[]{{dims(f1), 0.25}, {dims(f2), 0.5}, {dims(f3), 0.75}};
    require(verify_interpolated_sequence(exact, left, right), "oracle exact quarter/mid/three-quarter positive control");
    std::vector<float> blend(a.size()), blank(a.size());
    for (float t : {0.5f, 0.75f}) {
        for (size_t i = 0; i < a.size(); ++i) blend[i] = std::lerp(a[i], b[i], t);
        require(!verify_interpolated_square(dims(blend), left, right, t), "oracle accepted an alpha blend");
        require(!verify_interpolated_square(dims(blank), left, right, t), "oracle accepted blank output");
    }
    std::puts("oracle_exact=PASS alpha_50_50=REJECTED alpha_25_75=REJECTED blank=REJECTED");
}

inline bool open(AnrFgBackend& backend, Graphics& graphics, const std::filesystem::path& path,
    std::unique_ptr<IAnrGuideProvider> provider = {})
{
    return backend.open({graphics.gpu->d3d_device(), graphics.gpu->drawQueue.Get(), extent,
        1, path.native(), true, stderr, graphics.gpu.get()}, std::move(provider));
}

inline void hardware(const std::filesystem::path& dll, bool real)
{
    Graphics graphics(false);
    {
        AnrFgBackend backend;
        require(open(backend, graphics, dll), "open hardware ANR guide path");
        const auto a = square(left, 0xffffffffu, 0xff000000u);
        const auto b = square(right, 0xffffffffu, 0xff000000u);
        for (unsigned multiplier : {2u, 4u}) {
            graphics.transfer(graphics.gpu->input[0].Get(), &a);
            graphics.transfer(graphics.gpu->input[1].Get(), &b);
            const auto ticket = backend.try_submit_pair(pair(graphics, 1, 2 * multiplier, multiplier), 0, 1);
            require(ticket && ticket->activeMultiplier == multiplier, "hardware ANR group admission");
            backend.synchronize();
            std::array<std::vector<float>, 3> luminance;
            std::vector<nr::fg::qa::InterpolatedFrame> frames;
            for (unsigned sub = 1; sub < multiplier; ++sub) {
                require(backend.subframe_ready(*ticket, sub), "hardware ANR disable byte must be zero");
                auto* target = graphics.gpu->syntheticTex[sub - 1].Get();
                graphics.record([&](auto* cmd) { backend.resolve_subframe(cmd, *ticket, target, sub); });
                auto pixels = graphics.transfer(target);
                if (real) {
                    auto& luma = luminance[sub - 1];
                    for (const auto pixel : pixels) {
                        const float r = static_cast<float>(pixel & 255u), g = static_cast<float>((pixel >> 8) & 255u);
                        const float b0 = static_cast<float>((pixel >> 16) & 255u);
                        luma.push_back((0.2126f * r + 0.7152f * g + 0.0722f * b0) / 255.f);
                    }
                    frames.push_back({{static_cast<int>(extent.width), static_cast<int>(extent.height), luma},
                        static_cast<double>(sub) / multiplier});
                } else {
                    constexpr uint32_t expected[]{0xffbf4020u, 0xffbf4040u, 0xffbf4060u};
                    require(pixels.front() == expected[sub - 1] && pixels.back() == expected[sub - 1],
                        "hardware mock GPU output");
                }
            }
            if (real)
                require(nr::fg::qa::verify_interpolated_sequence(frames, left, right), "real B15 anti-blend oracle");
            backend.retire_pair(*ticket);
        }
    }
    graphics.verify_debug();
    std::puts(real ? "real_b15_interpolation=PASS" : "hardware_guides=PASS reset/flow/collect/destroy/close");
}
} // namespace anr_test
