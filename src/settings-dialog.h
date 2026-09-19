#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the (currently offline-only) settings dialog. Implemented in
 * settings-dialog.cpp because it needs Qt. Declared here in a plain header
 * so plain-C translation units (plugin-main.c) can call it without pulling
 * in any Qt headers. */
void tea_show_settings_dialog(void);

#ifdef __cplusplus
}
#endif
