// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Companion Intel runtime admission for the opt-in XeMFG path. The community
// provider is admitted by xemfg_dll_validator; this gate admits only the
// official Intel pair with xefg_runtime's signature and pinned-hash rules.
#include "xemfg_dll_validator.h"
#include <cstdio>
#include <memory>

namespace nr::fg::vfg {
// The returned validator owns no state beyond the log and hands back a lease
// that keeps both Intel images loaded for the provider's lifetime.
std::unique_ptr<xemfg::CompanionValidator> make_companion_gate(FILE* log);
} // namespace nr::fg::vfg
