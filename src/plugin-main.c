/*
tea-live-subtitle
Copyright (C) 2026 Caleb Weixun

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "captions-source.h"
#include "settings-dialog.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void tea_on_tools_menu_clicked(void *private_data)
{
	(void)private_data;
	tea_show_settings_dialog();
}

/* "Pause captions in these scenes": tell every source the program scene. */
static void tea_on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	(void)private_data;
	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED && event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;
	obs_source_t *scene = obs_frontend_get_current_scene();
	tea_captions_source_program_scene_changed(scene ? obs_source_get_name(scene) : "");
	obs_source_release(scene);
}

bool obs_module_load(void)
{
	tea_captions_source_register();

	obs_frontend_add_tools_menu_item(obs_module_text("TeaLiveSubtitle.Menu.SettingsTitle"),
					 tea_on_tools_menu_clicked, NULL);
	obs_frontend_add_event_callback(tea_on_frontend_event, NULL);

	obs_log(LOG_INFO, "loaded version %s", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(tea_on_frontend_event, NULL);
	tea_captions_source_shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
