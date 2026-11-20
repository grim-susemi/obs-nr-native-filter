// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "nr_contract.h"
#include <obs.h>

// Shared implementation for the integrated filter and existing standalone scenes.
namespace nr::obs_upscale {
struct Filter;
Filter* create(obs_data_t*, obs_source_t*);
void destroy(Filter*);
bool update(Filter*, obs_data_t*, bool enabled, bool keepSize = false);
void defaults(obs_data_t*);
void add_properties(obs_properties_t*);
Extent dimensions(const Filter&);
void render(Filter&);
}
