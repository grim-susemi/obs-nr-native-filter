// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_runtime.h"

namespace nr::qa {
struct CostDriver;
void open_cost_driver(std::shared_ptr<CostDriver>&,ID3D12Device*,const std::filesystem::path&);
runtime::Dispatch cost_driver_dispatch(const std::shared_ptr<CostDriver>&);
bool close_cost_driver(const std::shared_ptr<CostDriver>&);
}
