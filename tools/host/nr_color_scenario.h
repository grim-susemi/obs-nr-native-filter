// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_color.h"
#include <filesystem>
#include <ostream>
namespace nr::qa {
struct ColorProof { uint32_t cases,pixels; float maxNormalizedCodes; uint32_t maxLinearHalfUlps; };
// Host supplies its admitted device/queue. This function never picks hardware.
// Throws on failure; returned proof is color-boundary evidence, NOT Feature18.
ColorProof run_color_scenario(ID3D12Device*,ID3D12CommandQueue*,std::span<const std::byte> shader,std::ostream* evidence=nullptr);
}
bool nr_run_color_scenario(const std::filesystem::path& root);
void nr_color_refuse_unrelated();
