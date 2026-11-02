// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_recording_decode.h"
#include <span>
#include "ghost_fixture.h"
namespace nr::qa::bootstrap {
constexpr uint32_t width=1920,height=1080;
constexpr uint32_t max_content_id=24; // Bounded source ID range; widened from 16 with the recording row budget.
constexpr uint32_t landmark_pitch=22; // x pitch of the moving source landmark (24 wide, 8 wide detector window)
static_assert(800+max_content_id*landmark_pitch+24<=1360,"landmarks must stay inside the dark band left of the HDR reference patch");
static_assert(landmark_pitch>=20,"a detector window must not touch a neighbouring landmark");
struct Image {
    bool hdr;
    std::vector<std::array<float,4>> pixels;
};
std::array<float,4> fixture_pixel(uint32_t x,uint32_t y,uint32_t id,bool hdr);
std::vector<uint8_t> fixture_bytes(uint32_t id,bool hdr);
Image rgb_image(std::span<const uint8_t>,bool hdr,bool bgra=false,const char* stage="RGB");
ghost::Rgb packed_rgb(std::span<const uint8_t>,size_t pixel,bool half,bool gamma,bool bgra=false);
Image decoded_image(const DecodedFrame&,bool hdr);
uint32_t content_id(const Image&); // Two source-encoded bit rows AND independent moving landmark.
uint32_t content_id(const DecodedFrame&); // Actual luma codes, independent of RGB conversion.
uint64_t hash(std::span<const uint8_t>);
std::array<uint64_t,3> hash_three(const std::array<std::span<const uint8_t>,3>&);
bool parallel_hash_available();
std::array<uint64_t,8> hash_eight(const std::array<std::span<const uint8_t>,8>&,bool parallel=true);
bool finite_half(std::span<const uint8_t>);
void save_bmp(const std::filesystem::path&,const Image&);
uint32_t code(const DecodedFrame&,size_t plane,uint32_t x,uint32_t y);
std::vector<uint8_t> ghost_fixture_bytes(const ghost::Fixture&,unsigned tick,bool linearSource);
struct RawScanCounts {
    uint64_t matches=0,mismatches=0,special=0;
};
double ghost_raw_maximum(const ghost::Fixture&,unsigned tick,bool linearSource,
    std::span<const uint8_t>,double bound,RawScanCounts* counts=nullptr);
}
