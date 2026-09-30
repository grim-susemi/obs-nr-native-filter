// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifdef __cplusplus
#include "nr_contract.h"
namespace nr::sampler {
enum class Admission { Ready, NoTarget, ZeroExtent, UnsupportedRenderer, UnsupportedColor, CaptureFailed, ClockUnavailable };
inline Admission admit(bool target, bool d3d11, uint32_t width, uint32_t height, bool color)
{
    if (!target) return Admission::NoTarget;
    if (!width || !height) return Admission::ZeroExtent;
    if (!d3d11) return Admission::UnsupportedRenderer;
    return color ? Admission::Ready : Admission::UnsupportedColor;
}
inline uint64_t canonical_tick(uint64_t time, uint64_t interval)
{
    return time && interval ? time / interval + 1 : 0;
}
}
extern "C" {
#endif
void nr_register_obs_filter(void);
#ifdef __cplusplus
}
#endif
