/* netease_quality.h — play-quality resolution for netease source.
 *
 * Decides which quality level a song should be streamed at, following the
 * client convention "global default, per-song override":
 *
 *   per-song override (netease.quality.overrides)  →  else global
 *   netease.quality (config.json)  →  else "exhigh"
 *
 * The requested level is then checked against the song's *source table*
 * (song-music-quality: which tiers the track actually has a file for),
 * which is cached to avoid re-probing on every play. The cache holds only
 * song facts (immutable), never account entitlement (checked live).
 *
 * Storage split (matches the client's "preference vs cache" split):
 *   preference  → config.json (global) + quality_overrides.json (overrides)
 *   cache       → quality_cache.json  (~/.cache/netune, LRU, rebuildable)
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>

/* Resolve the effective level for `song_id`: override > global, then verify
   the level has a source (using the cached source table, probing + caching
   on miss). Returns a malloc'd level string, or NULL when the song has no
   usable source at/above the wanted level (caller decides how to react).
   On failure to probe, falls back to the requested level unverified. */
char *nq_resolve_level(const char *song_id);

/* Per-song override (persistent user preference). */
int nq_override_set(const char *song_id, const char *level);  /* 0 = ok */
int nq_override_get(const char *song_id, char *level, size_t sz); /* 0 = found */
int nq_override_del(const char *song_id);                     /* 0 = ok */

/* Source-table cache (rebuildable, LRU-capped). mask/br follow
   netease_song_music_quality() semantics (NQ_* bits, high→low). */
int nq_cache_get(const char *song_id, unsigned *mask, int *br); /* 0 = hit */
int nq_cache_put(const char *song_id, unsigned mask, const int *br); /* 0 = ok */

/* Global quality (config.json netease.quality), default "exhigh". */
const char *nq_global_level(void);
int nq_global_set(const char *level);  /* 0 = ok */

#ifdef __cplusplus
}
#endif
