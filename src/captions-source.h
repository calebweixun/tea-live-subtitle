#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the "tea_live_subtitle_source" obs_source_info with libobs.
 * Call once from obs_module_load(). */
void tea_captions_source_register(void);

#ifdef __cplusplus
}
#endif
