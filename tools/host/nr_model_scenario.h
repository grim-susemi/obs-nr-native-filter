// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include "nr_core_control_owner.h"
// 'retained' is the caller's existing unresolved-ownership flag: the model lane sets it when
// it cannot prove cleanup, so the coordinator takes the retained/supervision path instead of
// reaching obs_shutdown with retained=false.
bool nr_run_model_scenario(const std::filesystem::path&,bool& retained);
bool nr_start_core_control(const std::filesystem::path&,std::unique_ptr<nr::qa::CoreControlOwner>&);
