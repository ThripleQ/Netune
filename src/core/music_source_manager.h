#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "music_source.h"

/* Register a music source plugin */
int music_source_register(MusicSource *source);

/* Get a registered plugin by name */
MusicSource* music_source_get(const char *name);

/* Search across all registered sources (or a specific one).
   If source_name is NULL, searches all sources.
   Caller owns the SearchResult and must call search_result_free. */
int music_source_search(const char *source_name,
                        const char *keyword, int page, int page_size,
                        SearchResult *out);

/* Initialize all registered sources */
int music_source_manager_init(void);

/* Shutdown all registered sources */
void music_source_manager_shutdown(void);

/* Number of registered sources */
int music_source_count(void);

#ifdef __cplusplus
}
#endif
