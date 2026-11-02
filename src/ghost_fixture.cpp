// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_fixture.h"
#include <algorithm>
#include <stdexcept>

namespace nr::ghost {
namespace {
uint32_t next(uint32_t& state)
{
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
double scale(FixtureColor color)
{
    return color == FixtureColor::DarkSdr ? .0625 : 1;
}
}
Fixture fixture(uint32_t seed, FixtureColor color)
{
    if (seed != 0xA17 && seed != 0xB29) throw std::invalid_argument("disjoint frozen fixture seed required");
    Fixture f{seed, color, color == FixtureColor::Sdr || color == FixtureColor::DarkSdr ? 80.0 : 203.0, 1, {}};
    uint32_t state = seed;
    int position = 16, pan = 0;
    const double s = scale(color);
    Rgb paint{.75 * s, .25 * s, .5 * s};
    for (unsigned t = 0; t < Fixture::ticks; ++t) {
        const bool held = (t > 64 && t < 80) || (t > 192 && t < 208);
        const bool cut = t == 128 || t == 224;
        if (held) {
            auto prior = f.schedule.back();
            prior.ingress = t + 1; prior.held = true; prior.cut = false;
            f.schedule.push_back(prior);
            continue;
        }
        const auto random = next(state);
        const int speed = t < 32 ? 1 : t < 64 ? 8 : t < 112 ? 32 : t < 144 ? -1 : t < 176 ? -8 : -32;
        if (cut) {
            position = 16 + static_cast<int>(random % 113);
            paint = {(.5 + static_cast<double>((random >> 8) & 3) / 16) * s, .25 * s, .75 * s};
        } else position = 8 + ((position - 8 + speed + 144) % 144);
        if (t >= 80 && t < 128) pan += speed;
        f.schedule.push_back({t + 1, t + 1, pan, {position, 20, 32, 48},
            {160 - position, 28, 32, 32}, paint, cut, false, t >= 144 && t < 176});
    }
    return f;
}
bool inside(Box b, unsigned x, unsigned y)
{
    const int px = static_cast<int>(x), py = static_cast<int>(y);
    return px >= b.x && py >= b.y && px < b.x + b.width && py < b.y + b.height;
}
bool foreground(const Tick& t, unsigned x, unsigned y)
{
    const int sy = static_cast<int>(y) - t.silhouette.y;
    const int sx = static_cast<int>(x) - t.silhouette.x;
    return inside(t.bar, x, y) || (inside(t.silhouette, x, y) && (sy >= 8 || (sx >= 8 && sx < 24)));
}
bool visible(unsigned x, unsigned y) { return x < Fixture::width && y >= 8 && y < Fixture::height; }
Rgb clean_pixel(const Fixture& f,const Tick& t,unsigned x,unsigned y)
{
    const double s = scale(f.color);
        const unsigned phase = static_cast<unsigned>((static_cast<int>(x) + t.pan + 8192) % 8192);
        uint32_t textureState=f.seed^((phase+1)*0x9E3779B9u)^((y+1)*0x85EBCA6Bu);
        const double texture=static_cast<double>(next(textureState)&255)/255*.125;
        Rgb c{(.125 + texture) * s, (.25 + texture) * s, (.1875 + texture) * s};
        if (t.sameColoredReveal && y >= 28 && y < 60 && x >= 16 && x < 144) {
            c=t.foreground;
            if ((x+y)&1) {c.r+=.0625*s;c.g+=.0625*s;c.b+=.0625*s;}
        }
        if (foreground(t, x, y)) {
            c = t.foreground;
            if ((x + y) & 1) { c.r += .0625 * s; c.g += .0625 * s; c.b += .0625 * s; }
        }
        // One working-pixel HUD line. HDR highlights are fixed multiples of fixture white.
        if (y == 80) c = {s, s, s};
        if (y >= 84 && y < Fixture::height && x >= 160 && x < Fixture::width &&
            f.color != FixtureColor::Sdr && f.color != FixtureColor::DarkSdr)
            c = {4, 2, 1};
        // Source-encoded ID and its complement, plus spatially separated analytic landmarks.
        if (y < 4 && x < 64) {
            const unsigned bit = x / 4;
            const bool on = ((t.contentId >> bit) & 1) != 0;
            const double v = (y < 2 ? on : !on) ? .875 : .125;
            c = {v, v, v};
        }
        if (y == 6 && (x == 83 || x == 127 || x == 179)) {
            const unsigned code = (t.contentId * (x + 1) + f.seed) & 255;
            c = {static_cast<double>(code) / 256, static_cast<double>(255 - code) / 256, .375};
        }
    return c;
}
Image clean_image(const Fixture& f, unsigned tick, Extent canvas)
{
    if(canvas.width<Fixture::width||canvas.height<Fixture::height) throw std::invalid_argument("fixture canvas cannot downscale geometry");
    const auto& t=f.schedule.at(tick);
    Image result(static_cast<size_t>(canvas.width)*canvas.height);
    for(unsigned y=0;y<canvas.height;++y)for(unsigned x=0;x<canvas.width;++x)
        result[static_cast<size_t>(y)*canvas.width+x]=clean_pixel(f,t,x,y);
    return result;
}
Image inherited_image(const Fixture& f, unsigned tick, Extent canvas)
{
    auto result = clean_image(f, tick,canvas);
    const auto previous = clean_image(f, tick == 0 ? 0 : tick - 1,canvas);
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = {.9 * result[i].r + .1 * previous[i].r, .9 * result[i].g + .1 * previous[i].g,
                     .9 * result[i].b + .1 * previous[i].b};
    }
    return result;
}
unsigned decode_content(const Image& image, Extent canvas)
{
    if (canvas.width<Fixture::width||canvas.height<Fixture::height||
        image.size()!=static_cast<size_t>(canvas.width)*canvas.height) throw std::invalid_argument("source image extent");
    unsigned id = 0;
    for (unsigned bit = 0; bit < 16; ++bit) {
        const auto& a = image[bit * 4 + 1];
        const auto& b = image[2 * canvas.width + bit * 4 + 1];
        if (!std::isfinite(a.r) || !std::isfinite(b.r) || std::abs(a.r + b.r - 1) > .1 ||
            std::abs(a.r - b.r) < .5) throw std::invalid_argument("source landmark decode");
        if (a.r > b.r) id |= 1u << bit;
    }
    return id;
}
bool decoded_landmarks(const Fixture& f, unsigned tick, const Image& image, double epsilon, Extent canvas)
{
    if (canvas.width<Fixture::width||canvas.height<Fixture::height||
        image.size()!=static_cast<size_t>(canvas.width)*canvas.height||!std::isfinite(epsilon)||epsilon<0) return false;
    try { if (decode_content(image,canvas) != f.schedule.at(tick).contentId) return false; }
    catch (const std::invalid_argument&) { return false; }
    const auto clean = clean_image(f, tick);
    for (unsigned x : {83u, 127u, 179u}) {
        const auto a = image[6 * canvas.width + x], b = clean[6 * Fixture::width + x];
        if (!std::isfinite(a.r) || !std::isfinite(a.g) || !std::isfinite(a.b) ||
            std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)}) > epsilon) return false;
    }
    // Decode scene geometry independently of the header/three encoded landmarks.
    // This coarse source-color admission tolerates bounded edits, not relocation.
    const double geometryBound = .125 * scale(f.color);
    for (unsigned y = 20; y < 68; y += 4) for (unsigned x = 2; x < Fixture::width; x += 4) {
        const auto a = image[y * canvas.width + x], b = clean[y * Fixture::width + x];
        if (!std::isfinite(a.r) || !std::isfinite(a.g) || !std::isfinite(a.b) ||
            std::max({std::abs(a.r - b.r),std::abs(a.g - b.g),std::abs(a.b - b.b)}) > geometryBound) return false;
    }
    return true;
}
bool lineage(const Fixture& f, unsigned displayTick, OutputKind kind, std::span<const Capture> captures, double epsilon,
             const FrameKey& sourceKey, const FrameKey& guideReferenceKey,const FrameDescriptor* sourceDescriptor)
{
    if (displayTick >= Fixture::ticks || captures.empty() || !sourceKey.valid() || !guideReferenceKey.valid()) return false;
    const bool delayed = kind == OutputKind::DelayedRaw || kind == OutputKind::DelayedNr;
    if (delayed && displayTick == 0) return false;
    const unsigned inputTick = displayTick - (delayed ? 1 : 0);
    const bool cold=inputTick==0||f.schedule[inputTick].cut;
    if(guideReferenceKey.instanceId!=sourceKey.instanceId||
        guideReferenceKey.contentEpoch!=sourceKey.contentEpoch||
        guideReferenceKey.ingressSequence>sourceKey.ingressSequence||
        sourceKey.ingressSequence-guideReferenceKey.ingressSequence!=(cold?0u:1u)) return false;
    const auto canvas=sourceDescriptor?sourceDescriptor->allocation:Extent{Fixture::width,Fixture::height};
    if(sourceDescriptor&&(!sourceDescriptor->valid()||guideReferenceKey.instanceId!=sourceKey.instanceId||
        guideReferenceKey.ingressSequence>sourceKey.ingressSequence||
        sourceKey.ingressSequence-guideReferenceKey.ingressSequence>inputTick)) return false;
    std::array<bool, 9> seen{};
    const auto& expected=sourceKey; // Bound to immutable ingress by the production capture caller.
    std::optional<JobKey> job;
    for (const auto& c : captures) {
        const auto index = static_cast<size_t>(c.stage);
        if (index >= seen.size() || seen[index] || !c.descriptor.valid() || c.exposure != f.exposure ||
            (sourceDescriptor?c.descriptor!=*sourceDescriptor:c.descriptor.referenceWhiteNits!=f.referenceWhiteNits)||
            c.descriptor.allocation != canvas) return false;
        seen[index] = true;
        const bool reference = c.stage == Stage::GuideReference;
        const unsigned referenceTick=sourceDescriptor?
            inputTick-static_cast<unsigned>(sourceKey.ingressSequence-guideReferenceKey.ingressSequence):
            (cold?inputTick:inputTick-1);
        const unsigned imageTick = reference?referenceTick:inputTick;
        const auto& imageKey=reference?guideReferenceKey:expected;
        if (c.frame != imageKey || !decoded_landmarks(f, imageTick, c.pixels, epsilon,canvas)) return false;
        if(kind==OutputKind::DelayedNr&&(c.stage==Stage::Completed||c.stage==Stage::Selected||
            c.stage==Stage::ModelInput||c.stage==Stage::Unprotected)&&!c.job) return false;
        if (c.job) {
            if (!c.job->valid() || c.job->frame != expected || (job && *job != *c.job)) return false;
            job.emplace(*c.job);
        }
    }
    if (!seen[static_cast<size_t>(Stage::Raw)] || !seen[static_cast<size_t>(Stage::Selected)] ||
        !seen[static_cast<size_t>(Stage::Preencode)] || !seen[static_cast<size_t>(Stage::Decoded)]) return false;
    if (kind == OutputKind::DelayedNr && (!job || !seen[static_cast<size_t>(Stage::Completed)] ||
        !seen[static_cast<size_t>(Stage::ModelInput)] || !seen[static_cast<size_t>(Stage::GuideCurrent)] ||
        !seen[static_cast<size_t>(Stage::GuideReference)] || !seen[static_cast<size_t>(Stage::Unprotected)])) return false;
    const auto expectedColor = f.color == FixtureColor::Hdr2020 ? ColorSpace::Rec2020Linear :
        f.color == FixtureColor::Hdr709 ? ColorSpace::ScRgbLinear : ColorSpace::Rec709Srgb;
    if(!sourceDescriptor) for (const auto& c : captures) if (c.descriptor.color != expectedColor) return false;
    const auto original=std::find_if(captures.begin(),captures.end(),[](const Capture& c){return c.stage==Stage::Raw;});
    for(const auto& c:captures) if(c.descriptor!=original->descriptor) return false;
    if(!original->settingsRevision||original->buildIdentity.empty()) return false;
    for(const auto& c:captures)
        if(c.settingsRevision!=original->settingsRevision||c.buildIdentity!=original->buildIdentity) return false;
    return true;
}
uint64_t canonical_time(unsigned tick, unsigned numerator, unsigned denominator)
{
    if ((numerator != 60000 || denominator != 1001) && (numerator != 60 || denominator != 1))
        throw std::invalid_argument("fixture cadence");
    return static_cast<uint64_t>(tick) * 1000000000ull * denominator / numerator;
}
} // namespace nr::ghost
