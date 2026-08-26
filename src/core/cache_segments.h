/* cache_segments.h — ordered, non-overlapping segment list for the audio
 * cache's range map (M5: multi-segment cache, ExoPlayer CacheSpan style).
 *
 * A cache file may contain several valid byte regions separated by holes
 * (e.g. a sequential prefix plus a retained tail, with the gap still being
 * downloaded). This module tracks those regions as an ordered, non-
 * overlapping segment list so both the cache index and the playback path
 * can answer "is byte N valid?" and "which download should cover byte N?".
 *
 * The list is a fixed-capacity array (CACHE_SEG_MAX). Segments are merged
 * on insert when they are adjacent or overlapping, so the list always
 * contains the minimal set covering all recorded valid bytes. This module
 * is thread-agnostic: callers synchronize (the cache index uses its own
 * mutex; the playback path is single-threaded for segment edits).
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

#define CACHE_SEG_MAX 16

typedef struct {
    int64_t start;   /* first valid byte offset (>= 0) */
    int64_t len;     /* number of valid bytes */
} CacheSeg;

typedef struct {
    CacheSeg segs[CACHE_SEG_MAX];   /* sorted by start, non-overlapping */
    int      count;                 /* number of live segments */
} CacheSegList;

/* Reset to empty (count = 0). */
void cache_seglist_init(CacheSegList *l);

/* Insert [start, start+len) into the list, merging with adjacent/overlapping
 * segments. len <= 0 is a no-op. Returns the new segment count. */
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
