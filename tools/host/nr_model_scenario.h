// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include "nr_core_control_owner.h"
// 'retained' is the caller's existing unresolved-ownership flag: the model lane sets it when
// it cannot prove cleanup, so the coordinator takes the retained/supervision path instead of
// reaching obs_shutdown with retained=false.
enum class NrModelPassCase { Legacy, Two, CreationFailure, EvaluationFailure, Scale50, Odd50, Quality1, Quality2, Align100, Align50, Hdr50, Pair, Churn };
struct NrModelScenario {
    std::filesystem::path outputDigest;
    NrModelPassCase passCase=NrModelPassCase::Legacy;
};
bool nr_run_model_scenario(const std::filesystem::path&,bool& retained,const NrModelScenario& options={});
bool nr_start_core_control(const std::filesystem::path&,std::unique_ptr<nr::qa::CoreControlOwner>&);
