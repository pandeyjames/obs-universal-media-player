#include <obs-module.h>
#include <plugin-support.h>

#include "universal-media-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Resolve website video and live-stream links with bundled yt-dlp or Streamlink and play them as a native OBS source.";
}

bool obs_module_load(void)
{
	universal_media_init_process_cleanup();
	obs_register_source(&universal_media_source_info);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	universal_media_terminate_bundled_processes();
	obs_log(LOG_INFO, "plugin unloaded");
}
