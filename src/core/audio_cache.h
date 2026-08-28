#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>   /* int64_t for the cache byte regions */
#include "core/cache_segments.h"

/* audio_cache.h — transparent on-disk audio cache for streamed tracks.
 *
 * Cached files are plain audio (mp3/flac/...) stored under
 * <XDG_CACHE_HOME>/netune/audio, indexed by audio_cache.json in the
 * same directory. The cache is fully rebuildable (files are just copies
 * of the stream) and LRU-evicted by a configurable size cap
 * (cache.audio_limit_mb, default 2048). Lives entirely under the cache
 * root — never under the config/data root.
 *
 * The recording itself is done by ffmpeg_stream (tee to a .part file);
 * this module only manages the index + capacity + file naming. */

/* 0 = caching disabled via config (cache.audio_enabled=false) */
int  audio_cache_enabled(void);

/* cache directory (static buffer; parent dir created lazily) */
const char *audio_cache_dir(void);

/* Create the cache directory if it does not exist yet (mkdir -p). The
   stream downloader opens a .part file inside it, which fails if the
   directory is missing — call this before writing a new cache file. */
void audio_cache_ensure_dir(void);

/* Find a finished cache entry for song_id@quality.
   0 = hit: path_out filled with the full file path, *complete set to
   1 when the file covers the whole track (play directly) or 0 when it is
   only partially cached. *segs (may be NULL) receives the valid byte
   regions of a partial entry (sorted, non-overlapping; a whole-track file
   reports a single segment covering the whole size). complete may be NULL.
   -1 = miss (disabled, no entry, or existing entry at another quality —
   a quality mismatch means the cached copy must be re-downloaded). */
int  audio_cache_find(const char *song_id, const char *quality,
                      char *path_out, size_t sz, int *complete,
                      CacheSegList *segs);

/* Malloc'd full path of the in-progress .part recording file, or NULL
   when caching is disabled (caller then uses a plain stream). */
char *audio_cache_part_path(const char *song_id);

/* Malloc'd full path of the finished file (song_id + extension). */
char *audio_cache_final_path(const char *song_id, const char *ext);

/* Register a cache file at final_path (renamed from .part by the caller,
   or written in place by a partial-mode continuation). Records quality +
   size (stat'd internally) + now-ts, then prunes to the size cap.
   complete=1 for a whole-track file (segs ignored), 0 for a partial file
   with the given valid byte regions. Updating an existing entry (e.g.
   partial → complete) keeps its file. 0 = ok. */
int  audio_cache_commit(const char *song_id, const char *final_path,
                        const char *quality, int complete,
                        const CacheSegList *segs);

/* Refresh last-used timestamp after a cache-hit play. */
void audio_cache_touch(const char *song_id);

/* Drop one song's cache (index entry + file). Used when a re-cache at a
   different quality supersedes it. */
void audio_cache_remove(const char *song_id);

/* Delete every cached audio file (and any stray .part files) + reset the
   index. Returns the number of cache files removed. */
int  audio_cache_clear(void);

/* Total bytes currently cached (sum of index sizes). */
long long audio_cache_total_bytes(void);

/* Number of cache index entries. */
int audio_cache_entry_count(void);

/* Unix timestamp of the least-recently-used (oldest) entry; 0 if empty. */
long long audio_cache_oldest_ts(void);

/* Apply a new capacity limit (MB): write cache.audio_limit_mb to the
   config, then immediately LRU-prune so the cache fits the new cap.
   0 = ok. */
int audio_cache_set_limit_mb(int mb);

/* Apply a new entry-count limit: write cache.audio_max_entries to the
   config, then immediately LRU-prune so the cache fits the new count.
   0 = ok. */
int audio_cache_set_max_entries(int n);

/* Current entry-count limit from the config (default 200). */
int audio_cache_max_entries(void);

/* Reconcile the index against what is actually on disk after an unclean
   shutdown: drops entries whose file is missing / empty / truncated below
   the recorded prefix, and deletes leftover .part files (none are in the
   index, so without an active downloader every .part is crash residue).
   MUST be called while no cache file is actively being written (once at
   playback startup, before any stream opens). Returns 0. */
int audio_cache_reconcile(void);

#ifdef __cplusplus
}
#endif
