/* cache_segments.c — ordered, non-overlapping segment list (see header).
 *
 * Insertion walks the list to find the merge window and coalesces the new
 * [start, start+len) range with every overlapping or immediately-adjacent
 * segment, keeping the list minimal and sorted. Linear scan is fine: the
 * list stays small in practice (a seek storm is the pathological case) and
 * merges are rare. The backing array grows dynamically, so the segment
 * count has no fixed cap — a full list is never silently truncated.
 */
#include "core/cache_segments.h"

#include <stdlib.h>
#include <string.h>

void cache_seglist_init(CacheSegList *l) {
    if (!l) return;
    l->segs = (CacheSeg*)malloc(CACHE_SEG_INIT_CAP * sizeof(CacheSeg));
    l->cap = l->segs ? CACHE_SEG_INIT_CAP : 0;
    l->count = 0;
}

void cache_seglist_free(CacheSegList *l) {
    if (!l) return;
    free(l->segs);
    l->segs = NULL;
    l->count = 0;
    l->cap = 0;
}

int cache_seglist_add(CacheSegList *l, int64_t start, int64_t len) {
    if (!l || len <= 0 || start < 0) return l ? l->count : 0;
    int64_t end = start + len;

    int i = 0;
    /* skip segments entirely before the new range */
    while (i < l->count && l->segs[i].start + l->segs[i].len < start)
        i++;

    /* i now points at the first segment that overlaps or touches the range
       (or the insertion point). Coalesce everything from i while segments
       start <= end (adjacent counts as touching: seg.start == end merges). */
    int j = i;
    while (j < l->count && l->segs[j].start <= end) {
        int64_t seg_end = l->segs[j].start + l->segs[j].len;
        if (seg_end > end) end = seg_end;
        if (l->segs[j].start < start) start = l->segs[j].start;
        j++;
    }

    /* shift the tail left to close the gap left by the merged segments */
    int drop = j - i;
    if (drop > 0) {
        memmove(&l->segs[i + 1], &l->segs[j],
                (size_t)(l->count - j) * sizeof(CacheSeg));
        l->count -= (drop - 1);
    } else {
        /* fresh insertion: grow the backing array first, so the insert can
           never be dropped for lack of capacity (no segment loss) */
        if (l->count == l->cap) {
            int ncap = l->cap ? l->cap * 2 : CACHE_SEG_INIT_CAP;
            CacheSeg *ns = (CacheSeg*)realloc(l->segs,
                                              (size_t)ncap * sizeof(CacheSeg));
            if (!ns) return l->count;  /* OOM: keep existing list intact */
            l->segs = ns;
            l->cap = ncap;
        }
        memmove(&l->segs[i + 1], &l->segs[i],
                (size_t)(l->count - i) * sizeof(CacheSeg));
        l->count++;
    }
    l->segs[i].start = start;
    l->segs[i].len = end - start;
    return l->count;
}

int cache_seglist_contains(const CacheSegList *l, int64_t pos) {
    return cache_seglist_find(l, pos) >= 0;
}

int cache_seglist_find(const CacheSegList *l, int64_t pos) {
    if (!l || pos < 0) return -1;
    /* linear scan: tiny list, and segments are rare */
    for (int i = 0; i < l->count; i++) {
        if (pos < l->segs[i].start) return -1;      /* sorted: no later match */
        if (pos < l->segs[i].start + l->segs[i].len) return i;
    }
    return -1;
}

int64_t cache_seglist_total(const CacheSegList *l) {
    if (!l) return 0;
    int64_t total = 0;
    for (int i = 0; i < l->count; i++) total += l->segs[i].len;
    return total;
}
