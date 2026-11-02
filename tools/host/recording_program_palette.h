// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
namespace nr::qa {
struct ProgramPalette {
    // Frozen before candidate evaluation. Alpha is opaque in both formats.
    static constexpr std::array<std::array<uint8_t,4>,2> sdr{{{95,136,18,255},{114,113,190,255}}};
    static constexpr std::array<std::array<uint16_t,4>,2> hdr{{{11862,12361,11551,15360},{11436,12332,12935,15360}}};
};
}
