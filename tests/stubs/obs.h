#pragma once

/* Minimal libobs stand-in for OBS-free test builds. Only the opaque types that
 * the plugin's own headers mention are declared; nothing here is callable. */
typedef struct obs_source obs_source_t;
