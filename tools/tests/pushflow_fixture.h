// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fg_interpolation_oracle.h"
#include "../fg-presenter/pushflow_backend.h"
#include <vector>

namespace nr::fg::qa {
inline constexpr Square endpointA{80, 90, 12}, endpointB{128, 90, 12};
inline constexpr int fixtureWidth = 320, fixtureHeight = 192;

struct TestRun {
    unsigned failures{};
    void expect(bool ok, const char* label)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    }
};

inline void paint(std::vector<float>& pixels, int width, Square r, float peak)
{
    for (int y = r.y; y < r.y + r.width; ++y)
        for (int x = r.x; x < r.x + r.width; ++x)
            pixels[static_cast<size_t>(y * width + x)] = peak;
}

void real_pushflow(TestRun&, const std::filesystem::path&, unsigned expected);
} // namespace nr::fg::qa
