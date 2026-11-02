// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <vector>

namespace nr::ghost {
struct Rgb {
    double r, g, b;
    bool operator==(const Rgb&) const = default;
};
using Image = std::vector<Rgb>;
enum class FixtureColor { Sdr, DarkSdr, Hdr709, Hdr2020 };
struct Box { int x, y, width, height; };
struct Tick {
    unsigned ingress, contentId;
    int pan;
    Box bar, silhouette;
    Rgb foreground;
    bool cut, held, sameColoredReveal;
};
struct Fixture {
    static constexpr unsigned width = 192, height = 96, ticks = 256;
    uint32_t seed;
    FixtureColor color;
    double referenceWhiteNits, exposure;
    std::vector<Tick> schedule;
};
Fixture fixture(uint32_t seed, FixtureColor color);
Rgb clean_pixel(const Fixture&,const Tick&,unsigned x,unsigned y);
Image clean_image(const Fixture& f, unsigned tick, Extent canvas={Fixture::width,Fixture::height});
Image inherited_image(const Fixture& f, unsigned tick, Extent canvas={Fixture::width,Fixture::height});
bool inside(Box box, unsigned x, unsigned y);
bool foreground(const Tick& tick, unsigned x, unsigned y);
bool visible(unsigned x, unsigned y);
unsigned decode_content(const Image& image, Extent canvas={Fixture::width,Fixture::height});
bool decoded_landmarks(const Fixture& f, unsigned tick, const Image& image, double epsilon,
                       Extent canvas={Fixture::width,Fixture::height});
// Capture records are observations only; no scheduler/compositor implementation lives here.
enum class Stage { Raw, ModelInput, GuideCurrent, GuideReference, Unprotected, Completed, Selected, Preencode, Decoded };
struct Capture {
    Stage stage;
    FrameKey frame;
    std::optional<JobKey> job;
    FrameDescriptor descriptor;
    double exposure;
    Image pixels; // Declared linear primaries, normalized by fixed fixture white.
    uint64_t settingsRevision;
    std::string buildIdentity;
};
bool lineage(const Fixture& f, unsigned displayTick, OutputKind kind,
             std::span<const Capture> captures, double epsilon,
             const FrameKey& sourceKey, const FrameKey& guideReferenceKey,
             const FrameDescriptor* sourceDescriptor=nullptr);
uint64_t canonical_time(unsigned tick, unsigned numerator, unsigned denominator);
} // namespace nr::ghost
