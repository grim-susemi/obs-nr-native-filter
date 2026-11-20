// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

struct gs_texture;
using gs_texture_t = struct gs_texture;
struct ObsUpscaleDlss;

ObsUpscaleDlss* obs_upscale_dlss_create() noexcept;
// May join the worker; call outside the OBS graphics lock.
void obs_upscale_dlss_destroy(ObsUpscaleDlss*) noexcept;
// Graphics context required. Invalidate output and reset temporal history on
// the next accepted capture (e.g. a cut, source replacement, bypass or HDR).
void obs_upscale_dlss_reset(ObsUpscaleDlss*) noexcept;
// Graphics context required. Source is OBS SDR premultiplied RGBA/BGRA (or
// linear RGBA16F). Same-size input/output selects DLAA. Returns a borrowed
// linear premultiplied GS_RGBA16F texture for this draw, or nullptr for the
// immediate spatial path. Do not retain the returned pointer across draws.
// Model indices 0/1/2 select NGX presets K/M/L. A null or empty runtime
// path uses bin/64bit/dlss/nvngx_dlss.dll (relative to the OBS executable).
gs_texture_t* obs_upscale_dlss_render(ObsUpscaleDlss*, gs_texture_t* sdr_source,
                                    uint32_t output_width, uint32_t output_height,
                                    uint32_t model = 1, const char* runtime_path = nullptr) noexcept;
