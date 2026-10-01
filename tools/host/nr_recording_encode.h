// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <obs.h>
#include <filesystem>
#include <memory>
#include <vector>
namespace nr::qa {
struct PacketReceipt {
    int64_t pts, dts;
    int32_t timebaseNumerator, timebaseDenominator;
    size_t bytes;
    bool keyframe;
};
// A bounded real libobs encoded output. Elementary HEVC retains NVENC's SPS/VUI
// and avoids introducing another mux/inference process or global codec settings.
class Recording final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Recording(const std::filesystem::path&,uint32_t packets,bool hdr);
    ~Recording();
    Recording(const Recording&)=delete;
    Recording& operator=(const Recording&)=delete;
    void start();
    void first_packet();
    std::vector<PacketReceipt> finish();
};
}
