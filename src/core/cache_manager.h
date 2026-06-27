#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "music_source.h"
#include <stdbool.h>
#include <time.h>

/* ── Cache entry ──────────────────────────────────────
 * Each entry stores JSON-serialized data with an optional TTL.
 * ──────────────────────────────────────────────────── */

/* ── API ────────────────────────────────────────────── */

/* Initialize cache at cache_dir. Creates dir if missing. */
int  cache_init(const char *cache_dir);

/* Store metadata for a song. Returns 0 on success. */
int  cache_put_song(const char *source, const char *song_id,
                    const SongInfo *info, int ttl_sec);

/* Load cached metadata for a song.
 * Returns 0 if found and not expired, <0 if miss. */
int  cache_get_song(const char *source, const char *song_id,
                    SongInfo *out);

/* Search cached songs by keyword (matches title/artist/album).
 * Caller owns the SearchResult and must call search_result_free. */
int  cache_search(const char *keyword, int limit, SearchResult *out);

/* Store any key-value pair (JSON string). */
int  cache_put(const char *key, const char *json_value, int ttl_sec);

/* Load a cached entry. buf must be at least buf_size bytes.
 * Returns length written, or <0 on miss. */
int  cache_get(const char *key, char *buf, size_t buf_size);

/* Remove expired entries. Called on init and periodically. */
void cache_cleanup(void);

/* Clear entire cache. */
void cache_clear(void);

/* Shutdown and free resources. */
void cache_shutdown(void);

#ifdef __cplusplus
}
#endif
