#pragma once

#include "core/cover.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Song-list cover cache ─────────────────────────────
   The lyric-mode cover is a single object (current song). The song list
   needs MANY covers alive at once, loaded lazily as rows scroll into
   view. This module owns a bounded LRU of CoverData keyed by a stable
   hash of the cover URL.

   Loading is asynchronous: cover_cache_request() returns immediately and
   a worker thread fetches+decodes (serialized through a single worker so
   stb_image's stamp counter stays safe). When a requested cover is
   ready, EV_COVER_CACHE_LOADED is published with a CoverCacheResult; the
   main thread then calls cover_cache_store() to move it into the cache
   and reads it with cover_cache_get(). */

/* Payload of EV_COVER_CACHE_LOADED: a freshly fetched cover. `cd` holds
   worker-owned pixels; the main thread passes it to cover_cache_store()
   which moves them into the cache. */
typedef struct {
    uint64_t  id;
    CoverData cd;
} CoverCacheResult;

/* Request async load. Returns the stable id for the url (FNV-1a hash
   collapsed to 32 bits — kitty's image id is a positive 32-bit integer,
   0 on empty url). The caller should render the row with a placeholder
   and wait for EV_COVER_CACHE_LOADED before showing the cover. */
uint64_t cover_cache_request(const char *url);

/* Return the loaded cover for id, or NULL if not loaded yet / failed.
   The pointer is owned by the cache; valid until the entry is evicted. */
const CoverData *cover_cache_get(uint64_t id);

/* Main thread: move a worker-delivered CoverCacheResult into the cache.
   Call from the EV_COVER_CACHE_LOADED handler. */
void cover_cache_store(const CoverCacheResult *res);

/* Touch an entry (mark recently used). Cheap no-op when unknown. */
void cover_cache_touch(uint64_t id);

/* Forget all cached covers (free pixels) and stop the worker thread. */
void cover_cache_clear(void);

#ifdef __cplusplus
}
#endif
