// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ghost_fixture.h"

namespace nr::ghost {
struct Calibration {
    const double formatBound, rawMaxError, epsilon;
};
Calibration calibrate(double declaredFormatBound, std::span<const Image> clean, std::span<const Image> raw);
bool transport(const Image& clean, const Image& raw, double declaredBound);
struct TemplateScore {
    unsigned currentTick, priorTick, region;
    size_t pixels;
    double denominator, bound, incremental, raw, total;
    bool zeroContrast, pass;
};
struct TemporalReport {
    unsigned currentTick = 0;
    std::vector<TemplateScore> templates;
    double maximum = 0, maximumRaw = 0, maximumTotal = 0;
    bool pass = true;
};
// Unscaled working-pixel access after the caller validates the complete physical
// stage. The temporal masks do not require a dense full-canvas RGB conversion.
struct PixelView {
    const void* pixels;
    Extent extent;
    Rgb (*read)(const void*,unsigned x,unsigned y);
};
TemporalReport temporal(const Fixture&,unsigned tick,PixelView raw,PixelView output,const Calibration&);
TemporalReport temporal(const Fixture& f, unsigned tick, const Image& raw, const Image& output, const Calibration& calibration,
                        Extent canvas={Fixture::width,Fixture::height});
unsigned persistence(std::span<const TemporalReport> reports);
unsigned template_persistence(std::span<const TemporalReport> reports, unsigned priorTick, unsigned region);
struct Edge { size_t a, b; };
std::vector<Edge> detail_edges(const Fixture& f, unsigned tick);
bool detail(const Image& clean, const Image& output, std::span<const Edge> edges, double epsilon);
struct EditScore { double rms, retained; bool pass; };
EditScore edit(const Image& raw, const Image& output, const Image& unprotectedDelta,
               std::span<const size_t> roi, double epsilon);
struct GuideCalibration {
    const double mismatch, absoluteError, relativeError;
};
struct GuidePoint {
    Rgb current, warpedReference; // Actual BGRA8 analysis RGB / 255.
    double x, y, expectedX, expectedY;
    bool labeledVisible;
};
GuideCalibration calibrate_guides(std::span<const GuidePoint> points);
bool guide_point(const GuidePoint& point, const GuideCalibration& calibration);
bool valid_support(std::span<const bool> contributingSupport);
double decoded_vector(int16_t value, double activeScale);
} // namespace nr::ghost
