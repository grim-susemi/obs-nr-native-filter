// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <obs.h>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
namespace nr::qa {
struct StreamRate {
    uint32_t numerator=1,denominator=1;
    bool valid() const {
        return (numerator==1&&denominator==1)||(numerator==60&&denominator==1)||
            (numerator==60000&&denominator==1001);
    }
    std::string decoder_options() const {
        if(!valid())throw std::invalid_argument("recording declared rational stream rate");
        return "framerate="+std::to_string(numerator)+(denominator==1?"":"/"+std::to_string(denominator));
    }
    uint32_t decoder_observation_fps() const {return numerator==1?60u:600u;}
    bool timestamp_delta(uint64_t nanoseconds,uint32_t steps) const {
        if(!valid()||steps>32)return false;
        const uint64_t expected=static_cast<uint64_t>(steps)*denominator*1000000000ull/numerator;
        return nanoseconds>=expected?nanoseconds-expected<=1:expected-nanoseconds<=1;
    }
};
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
std::vector<DecodedFrame> decode_recording(const std::filesystem::path&,bool hdr,uint32_t expectedFrames,
    StreamRate rate={});
}
