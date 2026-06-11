#include "universal-media-source.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/dstr.h>
#include <util/pipe.h>
#include <util/platform.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#endif

#define S_URL "url"
#define S_PROVIDER "provider"
#define S_PLAYBACK_MODE "playback_mode"
#define S_ENGINE "resolver_engine"
#define S_RESOLVE "resolve_with_ytdlp"
#define S_QUALITY "quality"
#define S_FORMAT "format_selector"
#define S_STREAMLINK_STREAM "streamlink_stream"
#define S_GUIDANCE "provider_guidance"
#define S_COOKIES "cookies_browser"
#define S_LOOP "looping"
#define S_HW_DECODE "hw_decode"
#define S_CLOSE_INACTIVE "close_when_inactive"
#define S_RESTART_ACTIVE "restart_on_activate"
#define S_CLEAR_END "clear_on_media_end"
#define S_BUFFERING "buffering_mb"
#define S_RECONNECT_DELAY "reconnect_delay_sec"
#define S_STATUS "status"
#define S_SOURCE_INFO "source_info"
#define S_SCALING_MODE "scaling_mode"
#define S_AUTO_FIT "auto_fit"

#define SCALE_FIT "fit"
#define SCALE_STRETCH "stretch"
#define SCALE_CENTER "center"
#define SCALE_RESET "reset"

#define ENGINE_AUTO "auto"
#define ENGINE_YTDLP "ytdlp"
#define ENGINE_STREAMLINK "streamlink"
#define ENGINE_DIRECT "direct"

#define PLAYBACK_STREAM "stream"
#define PLAYBACK_DOWNLOAD "download"

struct universal_media {
	obs_source_t *source;
	obs_source_t *media;
	char *resolved_by;
	char *cache_path;
	bool active;
};

#ifdef _WIN32
struct os_process_pipe_windows {
	bool read_pipe;
	HANDLE handle;
	HANDLE handle_err;
	HANDLE process;
};

static HANDLE resolver_job;

static void normalize_path(char *path)
{
	for (char *ch = path; ch && *ch; ch++) {
		if (*ch == '\\')
			*ch = '/';
		else if (*ch >= 'A' && *ch <= 'Z')
			*ch = (char)(*ch - 'A' + 'a');
	}
}

static char *bundled_bin_directory(void)
{
	char *dir = obs_module_file("bin");
	if (dir)
		normalize_path(dir);
	return dir;
}

static bool process_path_in_bundled_bin(DWORD process_id, const char *bin_dir)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, process_id);
	if (!process)
		return false;

	wchar_t wide_path[MAX_PATH * 4];
	DWORD size = (DWORD)(sizeof(wide_path) / sizeof(wide_path[0]));
	bool match = false;

	if (QueryFullProcessImageNameW(process, 0, wide_path, &size)) {
		char *path = NULL;
		if (os_wcs_to_utf8_ptr(wide_path, 0, &path) && path) {
			normalize_path(path);
			match = strncmp(path, bin_dir, strlen(bin_dir)) == 0 &&
				(path[strlen(bin_dir)] == '/' || path[strlen(bin_dir)] == '\0');
			bfree(path);
		}
	}

	CloseHandle(process);
	return match;
}

static void terminate_bundled_process_tree(void)
{
	char *bin_dir = bundled_bin_directory();
	if (!bin_dir)
		return;

	DWORD current_process = GetCurrentProcessId();
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		bfree(bin_dir);
		return;
	}

	PROCESSENTRY32W entry = {0};
	entry.dwSize = sizeof(entry);

	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (entry.th32ProcessID == current_process)
				continue;

			if (!process_path_in_bundled_bin(entry.th32ProcessID, bin_dir))
				continue;

			HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
						     false, entry.th32ProcessID);
			if (process) {
				blog(LOG_INFO,
				     "[Universal Media Player] Terminating stale bundled process %lu",
				     (unsigned long)entry.th32ProcessID);
				TerminateProcess(process, 1);
				CloseHandle(process);
			}
		} while (Process32NextW(snapshot, &entry));
	}

	CloseHandle(snapshot);
	bfree(bin_dir);
}

static bool ensure_resolver_job(void)
{
	if (resolver_job)
		return true;

	resolver_job = CreateJobObjectW(NULL, NULL);
	if (!resolver_job) {
		blog(LOG_WARNING, "[Universal Media Player] Could not create resolver cleanup job: %lu",
		     GetLastError());
		return false;
	}

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(resolver_job, JobObjectExtendedLimitInformation, &limits,
				     sizeof(limits))) {
		blog(LOG_WARNING, "[Universal Media Player] Could not configure resolver cleanup job: %lu",
		     GetLastError());
		CloseHandle(resolver_job);
		resolver_job = NULL;
		return false;
	}

	return true;
}

static void assign_pipe_to_cleanup_job(os_process_pipe_t *pipe)
{
	if (!pipe || !ensure_resolver_job())
		return;

	struct os_process_pipe_windows *windows_pipe = (struct os_process_pipe_windows *)pipe;
	if (!AssignProcessToJobObject(resolver_job, windows_pipe->process)) {
		DWORD error = GetLastError();
		if (error != ERROR_ACCESS_DENIED)
			blog(LOG_WARNING,
			     "[Universal Media Player] Could not assign resolver process to cleanup job: %lu",
			     error);
	}
}
#endif

void universal_media_init_process_cleanup(void)
{
#ifdef _WIN32
	terminate_bundled_process_tree();
	ensure_resolver_job();
#endif
}

void universal_media_terminate_bundled_processes(void)
{
#ifdef _WIN32
	if (resolver_job) {
		CloseHandle(resolver_job);
		resolver_job = NULL;
	}

	terminate_bundled_process_tree();
#endif
}

static os_process_pipe_t *create_managed_process_pipe(const os_process_args_t *args, const char *type)
{
	os_process_pipe_t *pipe = os_process_pipe_create2(args, type);
#ifdef _WIN32
	assign_pipe_to_cleanup_job(pipe);
#endif
	return pipe;
}

static const char *media_state_name(enum obs_media_state state)
{
	switch (state) {
	case OBS_MEDIA_STATE_PLAYING:
		return obs_module_text("MediaStatePlaying");
	case OBS_MEDIA_STATE_OPENING:
		return obs_module_text("MediaStateOpening");
	case OBS_MEDIA_STATE_BUFFERING:
		return obs_module_text("MediaStateBuffering");
	case OBS_MEDIA_STATE_PAUSED:
		return obs_module_text("MediaStatePaused");
	case OBS_MEDIA_STATE_STOPPED:
		return obs_module_text("MediaStateStopped");
	case OBS_MEDIA_STATE_ENDED:
		return obs_module_text("MediaStateEnded");
	case OBS_MEDIA_STATE_ERROR:
		return obs_module_text("MediaStateError");
	default:
		return obs_module_text("MediaStateInactive");
	}
}

static void update_source_info(struct universal_media *context, obs_data_t *settings)
{
	struct obs_video_info ovi = {0};
	obs_get_video_info(&ovi);

	const char *provider = obs_data_get_string(settings, S_PROVIDER);
	const char *engine = context && context->resolved_by ? context->resolved_by
							     : obs_data_get_string(settings, S_ENGINE);
	const char *playback_mode = obs_data_get_string(settings, S_PLAYBACK_MODE);
	uint32_t width = context && context->media ? obs_source_get_width(context->media) : 0;
	uint32_t height = context && context->media ? obs_source_get_height(context->media) : 0;
	int64_t duration = context && context->media ? obs_source_media_get_duration(context->media) : 0;
	enum obs_media_state state =
		context && context->media ? obs_source_media_get_state(context->media) : OBS_MEDIA_STATE_NONE;

	struct dstr info = {0};
	dstr_printf(&info,
		    "%s: %s\n%s: %s\n%s: %s\n%s: %ux%u\n%s: %ux%u\n%s: %s\n%s: ",
		    obs_module_text("InfoProvider"), provider && *provider ? provider : "auto",
		    obs_module_text("InfoResolver"), engine && *engine ? engine : "auto",
		    obs_module_text("InfoPlaybackMode"),
		    astrcmpi(playback_mode, PLAYBACK_DOWNLOAD) == 0 ? obs_module_text("PlaybackDownload")
								    : obs_module_text("PlaybackStream"),
		    obs_module_text("InfoMediaSize"), width, height, obs_module_text("InfoCanvasSize"),
		    ovi.base_width, ovi.base_height, obs_module_text("InfoMediaState"), media_state_name(state),
		    obs_module_text("InfoDuration"));

	if (duration > 0) {
		int64_t total_seconds = duration / 1000;
		dstr_catf(&info, "%02lld:%02lld:%02lld", (long long)(total_seconds / 3600),
			  (long long)((total_seconds / 60) % 60), (long long)(total_seconds % 60));
	} else {
		dstr_cat(&info, obs_module_text("InfoLiveUnknown"));
	}

	if (context && context->cache_path && *context->cache_path)
		dstr_catf(&info, "\n%s: %s", obs_module_text("InfoCachedFile"), context->cache_path);

	obs_data_set_string(settings, S_SOURCE_INFO, info.array);
	dstr_free(&info);
}

static obs_sceneitem_t *current_scene_item(struct universal_media *context, obs_source_t **scene_source)
{
	*scene_source = obs_frontend_get_current_scene();
	if (!*scene_source)
		return NULL;

	obs_scene_t *scene = obs_scene_from_source(*scene_source);
	if (!scene)
		return NULL;

	return obs_scene_find_source_recursive(scene, obs_source_get_name(context->source));
}

static bool apply_scaling(struct universal_media *context, const char *mode)
{
	if (!context || !mode || !*mode)
		return false;

	obs_source_t *scene_source = NULL;
	obs_sceneitem_t *item = current_scene_item(context, &scene_source);
	if (!item) {
		obs_source_release(scene_source);
		return false;
	}

	struct obs_video_info ovi = {0};
	obs_get_video_info(&ovi);
	obs_sceneitem_defer_update_begin(item);

	if (astrcmpi(mode, SCALE_FIT) == 0 || astrcmpi(mode, SCALE_STRETCH) == 0) {
		struct obs_transform_info info = {0};
		vec2_set(&info.pos, 0.0f, 0.0f);
		vec2_set(&info.scale, 1.0f, 1.0f);
		vec2_set(&info.bounds, (float)ovi.base_width, (float)ovi.base_height);
		info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		info.rot = 0.0f;
		info.bounds_type =
			astrcmpi(mode, SCALE_STRETCH) == 0 ? OBS_BOUNDS_STRETCH : OBS_BOUNDS_SCALE_INNER;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		info.crop_to_bounds = false;
		obs_sceneitem_set_info2(item, &info);

		struct obs_sceneitem_crop crop = {0};
		obs_sceneitem_set_crop(item, &crop);
	} else if (astrcmpi(mode, SCALE_CENTER) == 0) {
		struct obs_transform_info info = {0};
		obs_sceneitem_get_info2(item, &info);
		info.alignment = OBS_ALIGN_CENTER;
		vec2_set(&info.pos, (float)ovi.base_width / 2.0f, (float)ovi.base_height / 2.0f);
		obs_sceneitem_set_info2(item, &info);
	} else if (astrcmpi(mode, SCALE_RESET) == 0) {
		struct obs_transform_info info = {0};
		vec2_set(&info.pos, 0.0f, 0.0f);
		vec2_set(&info.scale, 1.0f, 1.0f);
		info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		info.bounds_type = OBS_BOUNDS_NONE;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		obs_sceneitem_set_info2(item, &info);

		struct obs_sceneitem_crop crop = {0};
		obs_sceneitem_set_crop(item, &crop);
	}

	obs_sceneitem_defer_update_end(item);
	obs_source_release(scene_source);
	return true;
}

static const char *ump_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("UniversalMediaPlayer");
}

static void read_pipe(struct dstr *output, os_process_pipe_t *pipe, bool error_stream)
{
	uint8_t buffer[4096];
	size_t length;

	do {
		length = error_stream ? os_process_pipe_read_err(pipe, buffer, sizeof(buffer))
				      : os_process_pipe_read(pipe, buffer, sizeof(buffer));
		if (length)
			dstr_ncat(output, (const char *)buffer, length);
	} while (length);
}

static bool is_media_url(const char *line)
{
	return strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0 ||
	       strncmp(line, "rtmp://", 7) == 0 || strncmp(line, "rtsp://", 7) == 0 ||
	       strncmp(line, "srt://", 6) == 0;
}

static char *find_last_media_url(const char *output)
{
	char *copy = bstrdup(output ? output : "");
	char *line = strtok(copy, "\r\n");
	char *result = NULL;

	while (line) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (is_media_url(line)) {
			bfree(result);
			result = bstrdup(line);
		}
		line = strtok(NULL, "\r\n");
	}

	bfree(copy);
	return result;
}

static char *find_last_existing_path(const char *output)
{
	char *copy = bstrdup(output ? output : "");
	char *line = strtok(copy, "\r\n");
	char *result = NULL;

	while (line) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (*line && os_file_exists(line)) {
			bfree(result);
			result = bstrdup(line);
		}
		line = strtok(NULL, "\r\n");
	}

	bfree(copy);
	return result;
}

static const char *quality_format(obs_data_t *settings)
{
	const char *quality = obs_data_get_string(settings, S_QUALITY);
	if (astrcmpi(quality, "2160") == 0)
		return "best[height<=2160]/best";
	if (astrcmpi(quality, "1440") == 0)
		return "best[height<=1440]/best";
	if (astrcmpi(quality, "1080") == 0)
		return "best[height<=1080]/best";
	if (astrcmpi(quality, "720") == 0)
		return "best[height<=720]/best";
	if (astrcmpi(quality, "480") == 0)
		return "best[height<=480]/best";
	if (astrcmpi(quality, "360") == 0)
		return "best[height<=360]/best";
	if (astrcmpi(quality, "custom") == 0) {
		const char *custom = obs_data_get_string(settings, S_FORMAT);
		return custom && *custom ? custom : "best";
	}
	return "best";
}

static const char *download_format(obs_data_t *settings)
{
	const char *quality = obs_data_get_string(settings, S_QUALITY);
	if (astrcmpi(quality, "2160") == 0)
		return "bestvideo[height<=2160][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=2160]+bestaudio/best[height<=2160]/best";
	if (astrcmpi(quality, "1440") == 0)
		return "bestvideo[height<=1440][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=1440]+bestaudio/best[height<=1440]/best";
	if (astrcmpi(quality, "1080") == 0)
		return "bestvideo[height<=1080][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=1080]+bestaudio/best[height<=1080]/best";
	if (astrcmpi(quality, "720") == 0)
		return "bestvideo[height<=720][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=720]+bestaudio/best[height<=720]/best";
	if (astrcmpi(quality, "480") == 0)
		return "bestvideo[height<=480][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=480]+bestaudio/best[height<=480]/best";
	if (astrcmpi(quality, "360") == 0)
		return "bestvideo[height<=360][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=360]+bestaudio/best[height<=360]/best";
	if (astrcmpi(quality, "custom") == 0) {
		const char *custom = obs_data_get_string(settings, S_FORMAT);
		return custom && *custom ? custom : "bestvideo+bestaudio/best";
	}
	return "bestvideo[ext=mp4]+bestaudio[ext=m4a]/bestvideo+bestaudio/best";
}

static char *cache_directory(struct dstr *error)
{
	char *dir = obs_module_config_path("cache");
	if (!dir) {
		dstr_copy(error, obs_module_text("StatusCachePathFailed"));
		return NULL;
	}

	if (os_mkdirs(dir) == MKDIR_ERROR) {
		dstr_copy(error, obs_module_text("StatusCachePathFailed"));
		bfree(dir);
		return NULL;
	}

	return dir;
}

static void clear_download_cache(const char *uuid)
{
	if (!uuid || !*uuid)
		return;

	struct dstr error = {0};
	char *dir = cache_directory(&error);
	dstr_free(&error);
	if (!dir)
		return;

	struct dstr pattern = {0};
	dstr_printf(&pattern, "%s/%s.*", dir, uuid);

	os_glob_t *glob = NULL;
	if (os_glob(pattern.array, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (!glob->gl_pathv[i].directory)
				os_unlink(glob->gl_pathv[i].path);
		}
		os_globfree(glob);
	}

	dstr_free(&pattern);
	bfree(dir);
}

static char *run_ytdlp(obs_data_t *settings, const char *format, struct dstr *error)
{
	const char *url = obs_data_get_string(settings, S_URL);
	char *ytdlp_path = obs_module_file("bin/yt-dlp.exe");
	if (!ytdlp_path || !os_file_exists(ytdlp_path)) {
		dstr_copy(error, obs_module_text("StatusMissingYtDlp"));
		bfree(ytdlp_path);
		return NULL;
	}

	const char *cookies = obs_data_get_string(settings, S_COOKIES);
	os_process_args_t *args = os_process_args_create(ytdlp_path);
	os_process_args_add_arg(args, "--ignore-config");
	os_process_args_add_arg(args, "--no-warnings");
	os_process_args_add_arg(args, "--no-playlist");
	os_process_args_add_arg(args, "--no-js-runtimes");
	os_process_args_add_arg(args, "--get-url");
	os_process_args_add_arg(args, "--format");
	os_process_args_add_arg(args, format && *format ? format : "best");

	if (cookies && *cookies && astrcmpi(cookies, "none") != 0) {
		os_process_args_add_arg(args, "--cookies-from-browser");
		os_process_args_add_arg(args, cookies);
	}

	os_process_args_add_arg(args, url);
	os_process_pipe_t *pipe = create_managed_process_pipe(args, "r");
	os_process_args_destroy(args);
	bfree(ytdlp_path);

	if (!pipe) {
		dstr_copy(error, obs_module_text("StatusStartFailed"));
		return NULL;
	}

	struct dstr output = {0};
	struct dstr error_output = {0};
	read_pipe(&output, pipe, false);
	read_pipe(&error_output, pipe, true);
	int exit_code = os_process_pipe_destroy(pipe);
	char *resolved = find_last_media_url(output.array);

	if (!resolved) {
		if (error_output.len)
			dstr_copy_dstr(error, &error_output);
		else if (output.len)
			dstr_copy_dstr(error, &output);
		else
			dstr_printf(error, "%s (%d)", obs_module_text("StatusResolveFailed"), exit_code);
	}

	dstr_free(&output);
	dstr_free(&error_output);
	return resolved;
}

static char *run_ytdlp_download(obs_data_t *settings, obs_source_t *source, struct dstr *error)
{
	const char *url = obs_data_get_string(settings, S_URL);
	const char *uuid = obs_source_get_uuid(source);
	char *ytdlp_path = obs_module_file("bin/yt-dlp.exe");
	if (!ytdlp_path || !os_file_exists(ytdlp_path)) {
		dstr_copy(error, obs_module_text("StatusMissingYtDlp"));
		bfree(ytdlp_path);
		return NULL;
	}

	char *ffmpeg_path = obs_module_file("bin/streamlink/ffmpeg");
	if (!ffmpeg_path || !os_file_exists(ffmpeg_path)) {
		dstr_copy(error, obs_module_text("StatusMissingFfmpeg"));
		bfree(ytdlp_path);
		bfree(ffmpeg_path);
		return NULL;
	}

	char *dir = cache_directory(error);
	if (!dir) {
		bfree(ytdlp_path);
		bfree(ffmpeg_path);
		return NULL;
	}

	clear_download_cache(uuid);

	struct dstr output_template = {0};
	dstr_printf(&output_template, "%s/%s.%%(ext)s", dir, uuid);

	const char *cookies = obs_data_get_string(settings, S_COOKIES);
	os_process_args_t *args = os_process_args_create(ytdlp_path);
	os_process_args_add_arg(args, "--ignore-config");
	os_process_args_add_arg(args, "--no-warnings");
	os_process_args_add_arg(args, "--no-playlist");
	os_process_args_add_arg(args, "--no-js-runtimes");
	os_process_args_add_arg(args, "--force-overwrites");
	os_process_args_add_arg(args, "--ffmpeg-location");
	os_process_args_add_arg(args, ffmpeg_path);
	os_process_args_add_arg(args, "--merge-output-format");
	os_process_args_add_arg(args, "mp4");
	os_process_args_add_arg(args, "--print");
	os_process_args_add_arg(args, "after_move:filepath");
	os_process_args_add_arg(args, "--format");
	os_process_args_add_arg(args, download_format(settings));
	os_process_args_add_arg(args, "--output");
	os_process_args_add_arg(args, output_template.array);

	if (cookies && *cookies && astrcmpi(cookies, "none") != 0) {
		os_process_args_add_arg(args, "--cookies-from-browser");
		os_process_args_add_arg(args, cookies);
	}

	os_process_args_add_arg(args, url);
	os_process_pipe_t *pipe = create_managed_process_pipe(args, "r");
	os_process_args_destroy(args);
	bfree(ytdlp_path);
	bfree(ffmpeg_path);

	if (!pipe) {
		dstr_copy(error, obs_module_text("StatusStartFailed"));
		dstr_free(&output_template);
		bfree(dir);
		return NULL;
	}

	struct dstr output = {0};
	struct dstr error_output = {0};
	read_pipe(&output, pipe, false);
	read_pipe(&error_output, pipe, true);
	int exit_code = os_process_pipe_destroy(pipe);
	char *downloaded = find_last_existing_path(output.array);

	if (!downloaded) {
		struct dstr fallback = {0};
		dstr_printf(&fallback, "%s/%s.mp4", dir, uuid);
		if (os_file_exists(fallback.array))
			downloaded = bstrdup(fallback.array);
		dstr_free(&fallback);
	}

	if (!downloaded) {
		if (error_output.len)
			dstr_copy_dstr(error, &error_output);
		else if (output.len)
			dstr_copy_dstr(error, &output);
		else
			dstr_printf(error, "%s (%d)", obs_module_text("StatusDownloadFailed"), exit_code);
	}

	dstr_free(&output);
	dstr_free(&error_output);
	dstr_free(&output_template);
	bfree(dir);
	return downloaded;
}

static bool is_likely_live_download(obs_data_t *settings)
{
	const char *provider = obs_data_get_string(settings, S_PROVIDER);
	const char *url = obs_data_get_string(settings, S_URL);

	if (!url || !*url)
		return false;

	if (astrcmpi(provider, "twitch") == 0 || strstr(url, "twitch.tv/")) {
		return !strstr(url, "/videos/") && !strstr(url, "/clip/") && !strstr(url, "/clips/");
	}

	if (astrcmpi(provider, "kick") == 0 || strstr(url, "kick.com/")) {
		return !strstr(url, "/video/") && !strstr(url, "/videos/") && !strstr(url, "/clip/");
	}

	if (astrcmpi(provider, "youtube") == 0 || strstr(url, "youtube.com/") || strstr(url, "youtu.be/")) {
		return strstr(url, "youtube.com/live/") || strstr(url, "youtu.be/live/");
	}

	return false;
}

static const char *streamlink_limit(obs_data_t *settings)
{
	const char *quality = obs_data_get_string(settings, S_QUALITY);
	if (astrcmpi(quality, "2160") == 0)
		return ">2160p";
	if (astrcmpi(quality, "1440") == 0)
		return ">1440p";
	if (astrcmpi(quality, "1080") == 0)
		return ">1080p";
	if (astrcmpi(quality, "720") == 0)
		return ">720p";
	if (astrcmpi(quality, "480") == 0)
		return ">480p";
	if (astrcmpi(quality, "360") == 0)
		return ">360p";
	return NULL;
}

static char *run_streamlink(obs_data_t *settings, struct dstr *error)
{
	const char *url = obs_data_get_string(settings, S_URL);
	char *streamlink_path = obs_module_file("bin/streamlink/bin/streamlink.exe");
	if (!streamlink_path || !os_file_exists(streamlink_path)) {
		dstr_copy(error, obs_module_text("StatusMissingStreamlink"));
		bfree(streamlink_path);
		return NULL;
	}

	const char *quality = obs_data_get_string(settings, S_QUALITY);
	const char *stream = "best";
	if (astrcmpi(quality, "custom") == 0) {
		const char *custom = obs_data_get_string(settings, S_STREAMLINK_STREAM);
		if (custom && *custom)
			stream = custom;
	}

	os_process_args_t *args = os_process_args_create(streamlink_path);
	os_process_args_add_arg(args, "--config");
	os_process_args_add_arg(args, "NUL");
	os_process_args_add_arg(args, "--loglevel");
	os_process_args_add_arg(args, "error");
	os_process_args_add_arg(args, "--stream-url");

	const char *limit = streamlink_limit(settings);
	if (limit) {
		os_process_args_add_arg(args, "--stream-sorting-excludes");
		os_process_args_add_arg(args, limit);
	}

	os_process_args_add_arg(args, url);
	os_process_args_add_arg(args, stream);
	os_process_pipe_t *pipe = create_managed_process_pipe(args, "r");
	os_process_args_destroy(args);
	bfree(streamlink_path);

	if (!pipe) {
		dstr_copy(error, obs_module_text("StatusStreamlinkStartFailed"));
		return NULL;
	}

	struct dstr output = {0};
	struct dstr error_output = {0};
	read_pipe(&output, pipe, false);
	read_pipe(&error_output, pipe, true);
	int exit_code = os_process_pipe_destroy(pipe);
	char *resolved = find_last_media_url(output.array);

	if (!resolved) {
		if (error_output.len)
			dstr_copy_dstr(error, &error_output);
		else if (output.len)
			dstr_copy_dstr(error, &output);
		else
			dstr_printf(error, "%s (%d)", obs_module_text("StatusStreamlinkResolveFailed"),
				    exit_code);
	}

	dstr_free(&output);
	dstr_free(&error_output);
	return resolved;
}

static bool prefer_streamlink(obs_data_t *settings)
{
	const char *provider = obs_data_get_string(settings, S_PROVIDER);
	const char *url = obs_data_get_string(settings, S_URL);
	return astrcmpi(provider, "twitch") == 0 || astrcmpi(provider, "kick") == 0 ||
	       (url && (strstr(url, "twitch.tv/") || strstr(url, "kick.com/")));
}

static char *resolve_ytdlp_with_fallback(obs_data_t *settings, struct dstr *error)
{
	const char *format = quality_format(settings);
	char *resolved = run_ytdlp(settings, format, error);
	if (!resolved && strcmp(format, "best") != 0) {
		blog(LOG_WARNING, "[Universal Media Player] Preferred format unavailable; retrying best format");
		dstr_free(error);
		resolved = run_ytdlp(settings, "best", error);
	}
	return resolved;
}

static char *resolve_url(obs_data_t *settings, struct dstr *error, const char **used_engine)
{
	const char *url = obs_data_get_string(settings, S_URL);
	if (!url || !*url) {
		dstr_copy(error, obs_module_text("StatusEmptyUrl"));
		return NULL;
	}

	const char *provider = obs_data_get_string(settings, S_PROVIDER);
	const char *engine = obs_data_get_string(settings, S_ENGINE);
	if (!obs_data_get_bool(settings, S_RESOLVE) || astrcmpi(provider, "direct") == 0 ||
	    astrcmpi(engine, ENGINE_DIRECT) == 0) {
		*used_engine = ENGINE_DIRECT;
		return bstrdup(url);
	}

	if (astrcmpi(engine, ENGINE_STREAMLINK) == 0) {
		*used_engine = ENGINE_STREAMLINK;
		return run_streamlink(settings, error);
	}
	if (astrcmpi(engine, ENGINE_YTDLP) == 0) {
		*used_engine = ENGINE_YTDLP;
		return resolve_ytdlp_with_fallback(settings, error);
	}

	bool streamlink_first = prefer_streamlink(settings);
	char *resolved = streamlink_first ? run_streamlink(settings, error)
					  : resolve_ytdlp_with_fallback(settings, error);
	if (resolved)
		*used_engine = streamlink_first ? ENGINE_STREAMLINK : ENGINE_YTDLP;
	if (!resolved) {
		struct dstr first_error = {0};
		dstr_copy_dstr(&first_error, error);
		dstr_free(error);
		blog(LOG_WARNING, "[Universal Media Player] Primary resolver failed; trying fallback");
		resolved = streamlink_first ? resolve_ytdlp_with_fallback(settings, error)
					    : run_streamlink(settings, error);
		if (resolved)
			*used_engine = streamlink_first ? ENGINE_YTDLP : ENGINE_STREAMLINK;
		if (!resolved && first_error.len) {
			struct dstr fallback_error = {0};
			dstr_copy_dstr(&fallback_error, error);
			dstr_printf(error, "%s\n%s", first_error.array,
				    fallback_error.array ? fallback_error.array : "");
			dstr_free(&fallback_error);
		}
		dstr_free(&first_error);
	}
	return resolved;
}

static char *prepare_input(struct universal_media *context, obs_data_t *settings, struct dstr *error,
			   const char **used_engine, bool *is_local_file)
{
	const char *url = obs_data_get_string(settings, S_URL);
	if (!url || !*url) {
		dstr_copy(error, obs_module_text("StatusEmptyUrl"));
		return NULL;
	}

	const char *playback_mode = obs_data_get_string(settings, S_PLAYBACK_MODE);
	if (astrcmpi(playback_mode, PLAYBACK_DOWNLOAD) == 0) {
		if (is_likely_live_download(settings)) {
			dstr_copy(error, obs_module_text("StatusDownloadLiveUnsupported"));
			return NULL;
		}

		*is_local_file = true;
		*used_engine = "yt-dlp download";
		return run_ytdlp_download(settings, context->source, error);
	}

	*is_local_file = false;
	return resolve_url(settings, error, used_engine);
}

static void update_child(struct universal_media *context, obs_data_t *settings, const char *input,
			 bool is_local_file)
{
	obs_data_t *child_settings = obs_data_create();
	obs_data_set_bool(child_settings, "is_local_file", is_local_file);
	if (is_local_file) {
		obs_data_set_string(child_settings, "local_file", input);
		obs_data_set_string(child_settings, "input", "");
	} else {
		obs_data_set_string(child_settings, "input", input);
		obs_data_set_string(child_settings, "local_file", "");
	}
	obs_data_set_string(child_settings, "input_format", "");
	obs_data_set_bool(child_settings, "looping", obs_data_get_bool(settings, S_LOOP));
	obs_data_set_bool(child_settings, "hw_decode", obs_data_get_bool(settings, S_HW_DECODE));
	obs_data_set_bool(child_settings, "close_when_inactive",
			  is_local_file ? false : obs_data_get_bool(settings, S_CLOSE_INACTIVE));
	obs_data_set_bool(child_settings, "restart_on_activate",
			  is_local_file ? false : obs_data_get_bool(settings, S_RESTART_ACTIVE));
	obs_data_set_bool(child_settings, "clear_on_media_end", obs_data_get_bool(settings, S_CLEAR_END));
	obs_data_set_int(child_settings, "buffering_mb", obs_data_get_int(settings, S_BUFFERING));
	obs_data_set_int(child_settings, "reconnect_delay_sec",
			 obs_data_get_int(settings, S_RECONNECT_DELAY));
	obs_data_set_int(child_settings, "speed_percent", 100);

	if (!context->media) {
		struct dstr name = {0};
		dstr_printf(&name, "%s (Media)", obs_source_get_name(context->source));
		context->media = obs_source_create_private("ffmpeg_source", name.array, child_settings);
		dstr_free(&name);
		if (context->media && context->active)
			obs_source_add_active_child(context->source, context->media);
	} else {
		obs_source_update(context->media, child_settings);
	}

	if (context->media)
		obs_source_media_restart(context->media);
	obs_data_release(child_settings);
}

static void ump_update(void *data, obs_data_t *settings)
{
	struct universal_media *context = data;
	const char *url = obs_data_get_string(settings, S_URL);

	if (!url || !*url) {
		obs_data_set_string(settings, S_STATUS, obs_module_text("StatusEnterUrl"));
		return;
	}

	const char *playback_mode = obs_data_get_string(settings, S_PLAYBACK_MODE);
	obs_data_set_string(settings, S_STATUS,
			    astrcmpi(playback_mode, PLAYBACK_DOWNLOAD) == 0
				    ? obs_module_text("StatusDownloading")
				    : obs_module_text("StatusResolving"));
	struct dstr error = {0};
	const char *used_engine = NULL;
	bool is_local_file = false;
	char *resolved = prepare_input(context, settings, &error, &used_engine, &is_local_file);
	if (resolved) {
		bfree(context->resolved_by);
		context->resolved_by = bstrdup(used_engine ? used_engine : ENGINE_DIRECT);
		bfree(context->cache_path);
		context->cache_path = is_local_file ? bstrdup(resolved) : NULL;
		update_child(context, settings, resolved, is_local_file);
		if (obs_data_get_bool(settings, S_AUTO_FIT))
			apply_scaling(context, SCALE_FIT);
		update_source_info(context, settings);
		obs_data_set_string(settings, S_STATUS, obs_module_text("StatusReady"));
		blog(LOG_INFO, "[Universal Media Player] Resolved source URL successfully");
	} else {
		const char *message = error.array ? error.array : obs_module_text("StatusResolveFailed");
		obs_data_set_string(settings, S_STATUS, message);
		blog(LOG_ERROR, "[Universal Media Player] %s", message);
	}

	bfree(resolved);
	dstr_free(&error);
}

static void *ump_create(obs_data_t *settings, obs_source_t *source)
{
	struct universal_media *context = bzalloc(sizeof(*context));
	context->source = source;
	ump_update(context, settings);
	return context;
}

static void ump_destroy(void *data)
{
	struct universal_media *context = data;
	if (context->media && context->active)
		obs_source_remove_active_child(context->source, context->media);
	obs_source_release(context->media);
	bfree(context->resolved_by);
	bfree(context->cache_path);
	bfree(context);
}

static void ump_activate(void *data)
{
	struct universal_media *context = data;
	context->active = true;
	if (context->media)
		obs_source_add_active_child(context->source, context->media);
}

static void ump_deactivate(void *data)
{
	struct universal_media *context = data;
	if (context->media)
		obs_source_remove_active_child(context->source, context->media);
	context->active = false;
}

static void ump_render(void *data, gs_effect_t *effect)
{
	struct universal_media *context = data;
	UNUSED_PARAMETER(effect);
	if (context->media)
		obs_source_video_render(context->media);
}

static uint32_t ump_width(void *data)
{
	struct universal_media *context = data;
	return context->media ? obs_source_get_width(context->media) : 0;
}

static uint32_t ump_height(void *data)
{
	struct universal_media *context = data;
	return context->media ? obs_source_get_height(context->media) : 0;
}

static bool ump_audio_render(void *data, uint64_t *timestamp, struct obs_source_audio_mix *audio_output,
			     uint32_t mixers, size_t channels, size_t sample_rate)
{
	struct universal_media *context = data;
	struct obs_source_audio_mix child_audio;

	if (!context->media || obs_source_audio_pending(context->media))
		return false;

	uint64_t child_timestamp = obs_source_get_audio_timestamp(context->media);
	if (!child_timestamp)
		return false;

	obs_source_get_audio_mix(context->media, &child_audio);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t channel = 0; channel < channels; channel++) {
			float *output = audio_output->output[mix].data[channel];
			float *input = child_audio.output[mix].data[channel];
			memcpy(output, input, AUDIO_OUTPUT_FRAMES * sizeof(float));
		}
	}

	*timestamp = child_timestamp;
	UNUSED_PARAMETER(sample_rate);
	return true;
}

static void ump_enum_active(void *data, obs_source_enum_proc_t callback, void *param)
{
	struct universal_media *context = data;
	if (context->media && context->active)
		callback(context->source, context->media, param);
}

static void ump_enum_all(void *data, obs_source_enum_proc_t callback, void *param)
{
	struct universal_media *context = data;
	if (context->media)
		callback(context->source, context->media, param);
}

static bool refresh_clicked(obs_properties_t *properties, obs_property_t *property, void *data)
{
	struct universal_media *context = data;
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);
	if (!context)
		return false;

	obs_data_t *settings = obs_source_get_settings(context->source);
	ump_update(context, settings);
	obs_data_release(settings);
	return true;
}

static bool source_info_clicked(obs_properties_t *properties, obs_property_t *property, void *data)
{
	struct universal_media *context = data;
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);
	if (!context)
		return false;

	obs_data_t *settings = obs_source_get_settings(context->source);
	update_source_info(context, settings);
	obs_data_release(settings);
	return true;
}

static bool clear_cache_clicked(obs_properties_t *properties, obs_property_t *property, void *data)
{
	struct universal_media *context = data;
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);
	if (!context)
		return false;

	clear_download_cache(obs_source_get_uuid(context->source));
	bfree(context->cache_path);
	context->cache_path = NULL;

	obs_data_t *settings = obs_source_get_settings(context->source);
	obs_data_set_string(settings, S_STATUS, obs_module_text("StatusCacheCleared"));
	update_source_info(context, settings);
	obs_data_release(settings);
	return true;
}

static bool scaling_clicked(obs_properties_t *properties, obs_property_t *property, void *data)
{
	struct universal_media *context = data;
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);
	if (!context)
		return false;

	obs_data_t *settings = obs_source_get_settings(context->source);
	const char *mode = obs_data_get_string(settings, S_SCALING_MODE);
	bool applied = apply_scaling(context, mode);
	obs_data_set_string(settings, S_STATUS,
			    applied ? obs_module_text("StatusScalingApplied")
				    : obs_module_text("StatusSourceNotInCurrentScene"));
	update_source_info(context, settings);
	obs_data_release(settings);
	return true;
}

static const char *provider_guidance(const char *provider)
{
	if (astrcmpi(provider, "youtube") == 0)
		return obs_module_text("GuideYouTube");
	if (astrcmpi(provider, "twitch") == 0)
		return obs_module_text("GuideTwitch");
	if (astrcmpi(provider, "vimeo") == 0)
		return obs_module_text("GuideVimeo");
	if (astrcmpi(provider, "dailymotion") == 0)
		return obs_module_text("GuideDailymotion");
	if (astrcmpi(provider, "facebook") == 0)
		return obs_module_text("GuideFacebook");
	if (astrcmpi(provider, "kick") == 0)
		return obs_module_text("GuideKick");
	if (astrcmpi(provider, "tiktok") == 0)
		return obs_module_text("GuideTikTok");
	if (astrcmpi(provider, "twitter") == 0)
		return obs_module_text("GuideTwitter");
	if (astrcmpi(provider, "instagram") == 0)
		return obs_module_text("GuideInstagram");
	if (astrcmpi(provider, "streamable") == 0)
		return obs_module_text("GuideStreamable");
	if (astrcmpi(provider, "rumble") == 0)
		return obs_module_text("GuideRumble");
	if (astrcmpi(provider, "reddit") == 0)
		return obs_module_text("GuideReddit");
	if (astrcmpi(provider, "bilibili") == 0)
		return obs_module_text("GuideBilibili");
	if (astrcmpi(provider, "soundcloud") == 0)
		return obs_module_text("GuideSoundCloud");
	if (astrcmpi(provider, "other") == 0)
		return obs_module_text("GuideOther");
	if (astrcmpi(provider, "direct") == 0)
		return obs_module_text("GuideDirect");
	return obs_module_text("GuideAuto");
}

static void update_resolver_visibility(obs_properties_t *properties, obs_data_t *settings)
{
	const char *provider = obs_data_get_string(settings, S_PROVIDER);
	const char *playback_mode = obs_data_get_string(settings, S_PLAYBACK_MODE);
	const char *engine = obs_data_get_string(settings, S_ENGINE);
	const bool download = astrcmpi(playback_mode, PLAYBACK_DOWNLOAD) == 0;
	const bool direct = astrcmpi(provider, "direct") == 0 || astrcmpi(engine, ENGINE_DIRECT) == 0;
	const bool streamlink = astrcmpi(engine, ENGINE_STREAMLINK) == 0;
	const bool ytdlp = astrcmpi(engine, ENGINE_YTDLP) == 0;
	const bool custom = astrcmpi(obs_data_get_string(settings, S_QUALITY), "custom") == 0;

	obs_property_set_visible(obs_properties_get(properties, S_ENGINE), !download);
	obs_property_set_visible(obs_properties_get(properties, S_RESOLVE), !direct && !download);
	obs_property_set_visible(obs_properties_get(properties, S_QUALITY), !direct || download);
	obs_property_set_visible(obs_properties_get(properties, S_COOKIES), (!direct && !streamlink) || download);
	obs_property_set_visible(obs_properties_get(properties, S_FORMAT),
				 ((!direct && !streamlink) || download) && custom);
	obs_property_set_visible(obs_properties_get(properties, S_STREAMLINK_STREAM),
				 !download && !direct && !ytdlp && custom);
}

static bool playback_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	update_resolver_visibility(properties, settings);
	return true;
}

static bool provider_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const char *provider = obs_data_get_string(settings, S_PROVIDER);
	obs_property_t *guidance = obs_properties_get(properties, S_GUIDANCE);
	obs_property_set_description(guidance, provider_guidance(provider));
	update_resolver_visibility(properties, settings);
	return true;
}

static bool quality_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const char *quality = obs_data_get_string(settings, S_QUALITY);
	UNUSED_PARAMETER(quality);
	update_resolver_visibility(properties, settings);
	return true;
}

static bool engine_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	update_resolver_visibility(properties, settings);
	return true;
}

static obs_properties_t *ump_properties(void *data)
{
	struct universal_media *context = data;
	if (context) {
		obs_data_t *settings = obs_source_get_settings(context->source);
		update_source_info(context, settings);
		obs_data_release(settings);
	}

	obs_properties_t *properties = obs_properties_create();
	obs_property_t *providers = obs_properties_add_list(properties, S_PROVIDER, obs_module_text("Provider"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(providers, obs_module_text("ProviderAuto"), "auto");
	obs_property_list_add_string(providers, "YouTube", "youtube");
	obs_property_list_add_string(providers, "Twitch", "twitch");
	obs_property_list_add_string(providers, "Vimeo", "vimeo");
	obs_property_list_add_string(providers, "Dailymotion", "dailymotion");
	obs_property_list_add_string(providers, "Facebook", "facebook");
	obs_property_list_add_string(providers, "Kick", "kick");
	obs_property_list_add_string(providers, "TikTok", "tiktok");
	obs_property_list_add_string(providers, "X / Twitter", "twitter");
	obs_property_list_add_string(providers, "Instagram", "instagram");
	obs_property_list_add_string(providers, "Streamable", "streamable");
	obs_property_list_add_string(providers, "Rumble", "rumble");
	obs_property_list_add_string(providers, "Reddit", "reddit");
	obs_property_list_add_string(providers, "Bilibili", "bilibili");
	obs_property_list_add_string(providers, "SoundCloud", "soundcloud");
	obs_property_list_add_string(providers, obs_module_text("ProviderOther"), "other");
	obs_property_list_add_string(providers, obs_module_text("ProviderDirect"), "direct");
	obs_property_set_modified_callback(providers, provider_modified);

	obs_properties_add_text(properties, S_GUIDANCE, provider_guidance("auto"), OBS_TEXT_INFO);
	obs_properties_add_text(properties, S_URL, obs_module_text("MediaUrl"), OBS_TEXT_DEFAULT);

	obs_property_t *playback_modes = obs_properties_add_list(properties, S_PLAYBACK_MODE,
								obs_module_text("PlaybackMode"),
								OBS_COMBO_TYPE_LIST,
								OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(playback_modes, obs_module_text("PlaybackStream"), PLAYBACK_STREAM);
	obs_property_list_add_string(playback_modes, obs_module_text("PlaybackDownload"), PLAYBACK_DOWNLOAD);
	obs_property_set_modified_callback(playback_modes, playback_modified);

	obs_property_t *engines = obs_properties_add_list(properties, S_ENGINE, obs_module_text("ResolverEngine"),
							 OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(engines, obs_module_text("EngineAuto"), ENGINE_AUTO);
	obs_property_list_add_string(engines, "yt-dlp", ENGINE_YTDLP);
	obs_property_list_add_string(engines, "Streamlink", ENGINE_STREAMLINK);
	obs_property_list_add_string(engines, obs_module_text("EngineDirect"), ENGINE_DIRECT);
	obs_property_set_modified_callback(engines, engine_modified);

	obs_properties_add_bool(properties, S_RESOLVE, obs_module_text("ResolveWithYtDlp"));

	obs_property_t *quality = obs_properties_add_list(properties, S_QUALITY, obs_module_text("Quality"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(quality, obs_module_text("QualityBest"), "best");
	obs_property_list_add_string(quality, "2160p", "2160");
	obs_property_list_add_string(quality, "1440p", "1440");
	obs_property_list_add_string(quality, "1080p", "1080");
	obs_property_list_add_string(quality, "720p", "720");
	obs_property_list_add_string(quality, "480p", "480");
	obs_property_list_add_string(quality, "360p", "360");
	obs_property_list_add_string(quality, obs_module_text("QualityCustom"), "custom");
	obs_property_set_modified_callback(quality, quality_modified);
	obs_property_t *customFormat =
		obs_properties_add_text(properties, S_FORMAT, obs_module_text("FormatSelector"), OBS_TEXT_DEFAULT);
	obs_property_set_visible(customFormat, false);
	obs_property_t *streamlinkStream = obs_properties_add_text(
		properties, S_STREAMLINK_STREAM, obs_module_text("StreamlinkStream"), OBS_TEXT_DEFAULT);
	obs_property_set_visible(streamlinkStream, false);

	obs_property_t *cookies = obs_properties_add_list(properties, S_COOKIES, obs_module_text("BrowserCookies"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(cookies, obs_module_text("None"), "none");
	obs_property_list_add_string(cookies, "Microsoft Edge", "edge");
	obs_property_list_add_string(cookies, "Google Chrome", "chrome");
	obs_property_list_add_string(cookies, "Mozilla Firefox", "firefox");
	obs_property_list_add_string(cookies, "Brave", "brave");

	obs_properties_add_bool(properties, S_LOOP, obs_module_text("Loop"));
	obs_properties_add_bool(properties, S_HW_DECODE, obs_module_text("HardwareDecode"));
	obs_properties_add_bool(properties, S_CLOSE_INACTIVE, obs_module_text("CloseInactive"));
	obs_properties_add_bool(properties, S_RESTART_ACTIVE, obs_module_text("RestartActive"));
	obs_properties_add_bool(properties, S_CLEAR_END, obs_module_text("ClearEnd"));
	obs_properties_add_int_slider(properties, S_BUFFERING, obs_module_text("Buffering"), 0, 16, 1);
	obs_properties_add_int(properties, S_RECONNECT_DELAY, obs_module_text("ReconnectDelay"), 1, 60, 1);
	obs_properties_add_button2(properties, "refresh", obs_module_text("ResolveRefresh"), refresh_clicked, data);

	obs_property_t *status = obs_properties_add_text(properties, S_STATUS, obs_module_text("Status"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(status, OBS_TEXT_INFO_NORMAL);

	obs_properties_add_text(properties, S_SOURCE_INFO, obs_module_text("SourceInformation"), OBS_TEXT_INFO);
	obs_properties_add_button2(properties, "refresh_source_info", obs_module_text("RefreshSourceInformation"),
				   source_info_clicked, data);
	obs_properties_add_button2(properties, "clear_download_cache", obs_module_text("ClearDownloadCache"),
				   clear_cache_clicked, data);

	obs_property_t *scaling =
		obs_properties_add_list(properties, S_SCALING_MODE, obs_module_text("CanvasScaling"),
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(scaling, obs_module_text("ScalingFit"), SCALE_FIT);
	obs_property_list_add_string(scaling, obs_module_text("ScalingStretch"), SCALE_STRETCH);
	obs_property_list_add_string(scaling, obs_module_text("ScalingCenter"), SCALE_CENTER);
	obs_property_list_add_string(scaling, obs_module_text("ScalingReset"), SCALE_RESET);
	obs_properties_add_bool(properties, S_AUTO_FIT, obs_module_text("AutoFitAfterResolve"));
	obs_properties_add_button2(properties, "apply_scaling", obs_module_text("ApplyCanvasScaling"),
				   scaling_clicked, data);
	return properties;
}

static void ump_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_PROVIDER, "auto");
	obs_data_set_default_string(settings, S_PLAYBACK_MODE, PLAYBACK_STREAM);
	obs_data_set_default_string(settings, S_ENGINE, ENGINE_AUTO);
	obs_data_set_default_bool(settings, S_RESOLVE, true);
	obs_data_set_default_string(settings, S_QUALITY, "best");
	obs_data_set_default_string(settings, S_FORMAT, "best");
	obs_data_set_default_string(settings, S_STREAMLINK_STREAM, "best");
	obs_data_set_default_string(settings, S_COOKIES, "none");
	obs_data_set_default_bool(settings, S_LOOP, false);
	obs_data_set_default_bool(settings, S_HW_DECODE, true);
	obs_data_set_default_bool(settings, S_CLOSE_INACTIVE, false);
	obs_data_set_default_bool(settings, S_RESTART_ACTIVE, false);
	obs_data_set_default_bool(settings, S_CLEAR_END, false);
	obs_data_set_default_int(settings, S_BUFFERING, 4);
	obs_data_set_default_int(settings, S_RECONNECT_DELAY, 10);
	obs_data_set_default_string(settings, S_STATUS, obs_module_text("StatusEnterUrl"));
	obs_data_set_default_string(settings, S_SOURCE_INFO, obs_module_text("InfoUnavailable"));
	obs_data_set_default_string(settings, S_SCALING_MODE, SCALE_FIT);
	obs_data_set_default_bool(settings, S_AUTO_FIT, true);
}

static void ump_play_pause(void *data, bool pause)
{
	struct universal_media *context = data;
	if (context->media)
		obs_source_media_play_pause(context->media, pause);
}

static void ump_restart(void *data)
{
	struct universal_media *context = data;
	if (context->media)
		obs_source_media_restart(context->media);
}

static void ump_stop(void *data)
{
	struct universal_media *context = data;
	if (context->media)
		obs_source_media_stop(context->media);
}

static int64_t ump_duration(void *data)
{
	struct universal_media *context = data;
	return context->media ? obs_source_media_get_duration(context->media) : 0;
}

static int64_t ump_time(void *data)
{
	struct universal_media *context = data;
	return context->media ? obs_source_media_get_time(context->media) : 0;
}

static void ump_set_time(void *data, int64_t milliseconds)
{
	struct universal_media *context = data;
	if (context->media)
		obs_source_media_set_time(context->media, milliseconds);
}

static enum obs_media_state ump_state(void *data)
{
	struct universal_media *context = data;
	return context->media ? obs_source_media_get_state(context->media) : OBS_MEDIA_STATE_NONE;
}

struct obs_source_info universal_media_source_info = {
	.id = "universal_media_player",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = ump_get_name,
	.create = ump_create,
	.destroy = ump_destroy,
	.update = ump_update,
	.activate = ump_activate,
	.deactivate = ump_deactivate,
	.video_render = ump_render,
	.audio_render = ump_audio_render,
	.get_width = ump_width,
	.get_height = ump_height,
	.enum_active_sources = ump_enum_active,
	.enum_all_sources = ump_enum_all,
	.get_defaults = ump_defaults,
	.get_properties = ump_properties,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = ump_play_pause,
	.media_restart = ump_restart,
	.media_stop = ump_stop,
	.media_get_duration = ump_duration,
	.media_get_time = ump_time,
	.media_set_time = ump_set_time,
	.media_get_state = ump_state,
};
