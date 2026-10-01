// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_recording_decode.h"
#include <span>
namespace nr::qa::bootstrap {
constexpr uint32_t width=1920,height=1080;
struct Image {
    bool hdr;
    std::vector<std::array<float,4>> pixels;
};
std::array<float,4> fixture_pixel(uint32_t x,uint32_t y,uint32_t id,bool hdr);
std::vector<uint8_t> fixture_bytes(uint32_t id,bool hdr);
Image rgb_image(std::span<const uint8_t>,bool hdr,bool bgra=false,const char* stage="RGB");
Image decoded_image(const DecodedFrame&,bool hdr);
uint32_t content_id(const Image&); // Two source-encoded bit rows AND independent moving landmark.
uint32_t content_id(const DecodedFrame&); // Actual luma codes, independent of RGB conversion.
uint64_t hash(std::span<const uint8_t>);
void save_bmp(const std::filesystem::path&,const Image&);
uint32_t code(const DecodedFrame&,size_t plane,uint32_t x,uint32_t y);
}
