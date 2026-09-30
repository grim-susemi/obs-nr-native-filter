/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <obs-module.h>
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-nr-filter", "en-US")
MODULE_EXPORT const char *obs_module_name(void) { return "obs-nr-filter"; }
MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native NR foundation; no filter or inference registered yet";
}
bool obs_module_load(void)
{
    blog(LOG_INFO, "obs-nr-filter: foundation loaded; inference unavailable");
    return true;
}
void obs_module_unload(void) { blog(LOG_INFO, "obs-nr-filter: foundation unloaded"); }
