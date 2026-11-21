// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <obs.h>

namespace nr::split {
void defaults(obs_data_t*);
void add_properties(obs_properties_t*);

// One per participating OBS filter. The count only gates the normal render path.
class Setting {
    bool enabled_ = false;
public:
    ~Setting();
    void update(obs_data_t*);
};

struct Plan {
    obs_source_t* parent{};
    uint32_t percent{};
    explicit operator bool() const { return parent != nullptr; }
};
Plan plan(obs_source_t*);
// Defensive for clients rendering a filter directly instead of its parent.
bool capturing_original(obs_source_t*);
bool capturing_processed(obs_source_t*);

class Suppress {
    obs_source_t* previous_;
public:
    explicit Suppress(const Plan&);
    ~Suppress();
};

using Draw = void (*)(void*);
class Buffer {
    gs_texrender_t* target_{};
public:
    void clear();
    gs_texture_t* capture(Extent, gs_color_space, Draw, void*);
};

class Canvas {
    Buffer before_, after_, scaled_;
    gs_effect_t* effect_{};
    bool effectAttempted_{};
public:
    void clear();
    gs_texture_t* original(const Plan&, Extent, gs_color_space, bool scale);
    bool draw(gs_texture_t* before, gs_texture_t* after, Extent, uint32_t percent);
    bool render(const Plan&, Extent, Draw, void*);
};
}
