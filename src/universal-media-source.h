#pragma once

#include <obs.h>

extern struct obs_source_info universal_media_source_info;

void universal_media_init_process_cleanup(void);
void universal_media_terminate_bundled_processes(void);
