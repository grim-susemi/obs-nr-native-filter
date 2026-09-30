/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <obs-module.h>
#include "obs_filter.h"
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-nr-filter", "en-US")
MODULE_EXPORT const char *obs_module_name(void) { return "obs-nr-filter"; }
MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native NR source sampler; pass-through only, inference unavailable";
}
bool obs_module_load(void)
{
    nr_register_obs_filter();
    blog(LOG_INFO, "obs-nr-filter: susemi_nr_filter registered; inference unavailable");
    return true;
}
void obs_module_unload(void) { blog(LOG_INFO, "obs-nr-filter: foundation unloaded"); }
