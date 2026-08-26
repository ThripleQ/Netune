/* cache_segments.h — ordered, non-overlapping segment list for the audio
 * cache's range map (M5: multi-segment cache, ExoPlayer CacheSpan style).
 *
 * A cache file may contain several valid byte regions separated by holes
 * (e.g. a sequential prefix plus a retained tail, with the gap still being
 * downloaded). This module tracks those regions as an ordered, non-
 * overlapping segment list so both the cache index and the playback path
 * can answer "is byte N valid?" and "which download should cover byte N?".
 *
 * Segments are merged on insert when they are adjacent or overlapping, so
 * the list always contains the minimal set covering all recorded valid
 * bytes. The list grows dynamically (heap array, doubling capacity) — the
 * segment count has NO fixed cap, matching ExoPlayer's NavigableSet<TreeSet>
 * which bounds disk space via LRU eviction, never the span count. A seek
 * storm can fragment a file into many spans; a hard cap would silently drop
 * a valid region, which the playback path would then misread as a hole.
 *
 * Memory contract: init() allocates the backing array, free() releases it.
 * Every init() must be paired with a free() (callers own their lists; the
 * audio-cache index stores one list per entry, the playback path uses a
 * global list plus stack locals). This module is thread-agnostic: callers
 * synchronize (the cache index uses its own mutex; the playback path is
 * single-threaded for segment edits).
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

/* initial heap capacity; grows by doubling on demand */
#define CACHE_SEG_INIT_CAP 16

typedef struct {
    int64_t start;   /* first valid byte offset (>= 0) */
    int64_t len;     /* number of valid bytes */
} CacheSeg;

typedef struct {
    CacheSeg *segs;  /* heap array, sorted by start, non-overlapping */
    int      count;  /* number of live segments */
    int      cap;    /* allocated capacity (>= count) */
} CacheSegList;

/* Initialize an empty list (allocates the backing array; may leave
   segs=NULL with cap=0 on OOM — add() then retries the allocation). */
void cache_seglist_init(CacheSegList *l);

/* Release the backing array and reset to empty. Safe on an initialized
   list even if add() never ran; callers must NOT use the list after this
   without a fresh init(). */
void cache_seglist_free(CacheSegList *l);

/* Insert [start, start+len) into the list, merging with adjacent/overlapping
 * segments. len <= 0 is a no-op. Grows the array when full. Returns the new
 * segment count. */
int cache_seglist_add(CacheSegList *l, int64_t start, int64_t len);

/* 1 if pos lies inside one of the segments, 0 otherwise. */
int cache_seglist_contains(const CacheSegList *l, int64_t pos);

/* Index of the segment containing pos, or -1. */
int cache_seglist_find(const CacheSegList *l, int64_t pos);

/* Total number of valid bytes covered by all segments (sum of lens). */
int64_t cache_seglist_total(const CacheSegList *l);

#ifdef __cplusplus
}
#endif
