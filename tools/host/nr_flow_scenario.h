// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_guides.h"
#include <ostream>
#include <filesystem>
namespace nr::qa {
// Called only by admitted task-owned host on its owned worker device/queue.
// FALSE admission exits before ANY device/API call. No device creation or OBS-context access here.
int run_flow_translation(ID3D12Device*,ID3D12CommandQueue*,std::span<const std::byte> guideShader,
    bool hardwareLaneAuthorized,std::ostream& evidence);
}
bool nr_run_flow_scenario(const std::filesystem::path& ownedHostRoot);
