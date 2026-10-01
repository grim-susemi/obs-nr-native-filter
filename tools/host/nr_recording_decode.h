// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <obs.h>
#include <array>
#include <filesystem>
#include <vector>
namespace nr::qa {
// Exact decoder-delivered planes, not program captures or post-composition stamps.
struct DecodedFrame {
    uint32_t width, height;
    uint64_t timestamp;
    video_format format;
    video_trc transfer;
    bool fullRange;
    uint16_t maxLuminance;
    std::array<float,16> matrix;
    std::array<float,3> rangeMin, rangeMax;
    std::array<uint32_t,3> rowBytes, rows;
    std::array<std::vector<uint8_t>,3> planes;
};
// Caller first admits/loads the pinned official obs-ffmpeg module. This bounded
// source filter observes decoder planes during libobs async_tick, AFTER the
// production graph/encoder retire. Every expected actual frame and media-ended
// must arrive; a presentation skip is a failure, not a decoded-frame substitute.
std::vector<DecodedFrame> decode_recording(const std::filesystem::path&,bool hdr,uint32_t expectedFrames);
}
