// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace nr::fg::qa {
struct Square {
    int x, y, width; // top-left pixel, extent in pixels
};
struct LuminanceFrame {
    int width, height;
    std::span<const float> pixels; // row major, peak = 1
    float at(int x, int y) const
    {
        return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    }
    bool valid() const
    {
        return width > 0 && height > 0 && pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};
struct SquareSample { double centroidX; Square region; };

inline Square square_at(Square a, Square b, double t)
{
    return {static_cast<int>(std::lround(std::lerp(a.x, b.x, t))), a.y, a.width};
}

inline std::optional<SquareSample> verify_interpolated_square(LuminanceFrame frame, Square a, Square b, double t)
{
    if (!frame.valid() || !(t > 0 && t < 1) || a.width <= 0 || a.width != b.width || a.y != b.y ||
        std::abs(double(b.x) - a.x) < 4.0 * a.width) return {};
    const Square mid = square_at(a, b, t);
    const auto inside = [&](Square r) {
        return r.x >= 0 && r.y >= 0 && r.x <= frame.width - r.width && r.y <= frame.height - r.width;
    };
    if (!inside(a) || !inside(b) || !inside(mid) ||
        std::abs(mid.x - a.x) < a.width || std::abs(mid.x - b.x) < a.width) return {};
    double blendError{};
    const auto mean = [&](Square r, double blend) {
        double sum{};
        for (int y = r.y; y < r.y + r.width; ++y) {
            for (int x = r.x; x < r.x + r.width; ++x) {
                const double v = frame.at(x, y);
                sum += v;
                blendError += std::abs(v - blend);
            }
        }
        return sum / (double(r.width) * r.width);
    };
    const double occupancy = mean(mid, 0), ghostA = mean(a, 1 - t), ghostB = mean(b, t);
    if (!(occupancy >= 0.60 && ghostA <= 0.25 && ghostB <= 0.25 && ghostA < 0.50 * occupancy &&
        ghostB < 0.50 * occupancy && blendError / (3.0 * a.width * a.width) > 0.20)) return {};
    double sumX{}, sumY{}, mass{};
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const double v = frame.at(x, y);
            if (!std::isfinite(v) || v < 0 || v > 1) return {};
            if (v < 0.50) continue;
            sumX += (x + 0.5) * v;
            sumY += (y + 0.5) * v;
            mass += v;
        }
    }
    if (!mass) return {};
    const auto center = a.width / 2.0;
    const double x = sumX / mass, y = sumY / mass;
    if (std::abs(x - (std::lerp(a.x, b.x, t) + center)) > a.width / 4.0 ||
        std::abs(y - (a.y + center)) > a.width / 4.0) return {};
    return SquareSample{x, mid};
}

struct InterpolatedFrame { LuminanceFrame frame; double t; };

inline bool verify_interpolated_sequence(std::span<const InterpolatedFrame> frames, Square a, Square b)
{
    if (frames.empty()) return false;
    std::optional<SquareSample> previous;
    LuminanceFrame prior{};
    const auto ordered = [&](double first, double second) { return a.x < b.x ? first < second : first > second; };
    for (const auto& f : frames) {
        const auto sample = verify_interpolated_square(f.frame, a, b, f.t);
        if (!sample || !ordered(a.x + a.width / 2.0, sample->centroidX) ||
            !ordered(sample->centroidX, b.x + b.width / 2.0)) return false;
        if (previous) {
            if (!ordered(previous->centroidX, sample->centroidX) ||
                prior.width != f.frame.width || prior.height != f.frame.height) return false;
            if (frames.size() == 2) {
                const auto l = previous->region, r = sample->region;
                if (std::abs(l.x - r.x) < l.width) return false;
                double difference{};
                for (const auto region : {l, r})
                    for (int y = region.y; y < region.y + region.width; ++y)
                        for (int x = region.x; x < region.x + region.width; ++x)
                            difference += std::abs(f.frame.at(x, y) - prior.at(x, y));
                if (difference <= 0.20 * 2.0 * l.width * l.width) return false;
            }
        }
        previous = sample;
        prior = f.frame;
    }
    return true;
}
} // namespace nr::fg::qa
