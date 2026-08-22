#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "core/music_source.h"

/* Register the local file music source plugin */
void local_source_register(void);
MusicSource* local_source_create(void);

/* Ensure `dir` is present in music_sources.local.dirs (config, persisted)
   and rescan the local cache so newly downloaded files show up
   immediately. 0 = ok, -1 = config unavailable. */
int  local_register_download_dir(const char *dir);

#ifdef __cplusplus
}
#endif
