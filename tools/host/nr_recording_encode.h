// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <obs.h>
#include "recording_packet_transport.h"
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
    static std::vector<std::unique_ptr<Impl>>& retained_owners();
#ifdef NR_RECORDING_CPU_TESTS
    friend struct RecordingCpuProbe;
    explicit Recording(std::unique_ptr<Impl>);
#endif
public:
    static bool uncertain_ownership();
    bool close_stop(uint32_t bound=30000);
    Recording(const std::filesystem::path&,uint32_t packets,bool hdr);
    ~Recording();
    Recording(const Recording&)=delete;
    Recording& operator=(const Recording&)=delete;
    void start();
    void first_packet();
    std::vector<PacketReceipt> finish();
    const std::vector<PacketTransportReceipt>& transport() const;
    void observe_stop(void (*callback)(void*),void* owner);
};
bool recording_has_uncertain_owners();
bool retain_recording_failure(std::unique_ptr<Recording>&,uint32_t bound=30000);
}
