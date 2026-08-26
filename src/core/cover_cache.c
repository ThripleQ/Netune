#include "core/cover_cache.h"
#include "core/event_bus.h"
#include "infra/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── FNV-1a hash: stable id for a cover URL ──────────────
   Collapsed to 32 bits: kitty's image id (the `i` parameter) is a
   positive 32-bit integer, so a full 64-bit hash would be rejected by
   the terminal and the placement silently dropped. The id is never 0
   (0 is reserved by the protocol). A 32-bit space is plenty for the ~24
   live covers. */
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    uint64_t id = h & 0xFFFFFFFFULL;
    if (id == 0) id = 1;
    return id;
}

/* ── LRU cache ────────────────────────────────────────── */
#define COVER_CACHE_MAX 24

typedef struct {
    uint64_t    id;
    CoverData   cd;
    int         loaded;   /* 1 = cd holds valid pixels */
    int         failed;   /* 1 = last load failed (retry suppressed) */
    uint64_t    last_use; /* monotonic clock for LRU eviction */
} CoverCacheEntry;

static CoverCacheEntry g_entries[COVER_CACHE_MAX];
static size_t         g_n = 0;
static uint64_t       g_clock = 0;

static CoverCacheEntry *entry_find(uint64_t id) {
    for (size_t i = 0; i < g_n; i++)
        if (g_entries[i].id == id) return &g_entries[i];
    return NULL;
}

static CoverCacheEntry *entry_slot(void) {
    if (g_n < COVER_CACHE_MAX)
        return &g_entries[g_n++];
    /* evict the least-recently-used entry */
    size_t lru = 0;
    for (size_t i = 1; i < g_n; i++)
        if (g_entries[i].last_use < g_entries[lru].last_use)
            lru = i;
    CoverCacheEntry *e = &g_entries[lru];
    cover_free(&e->cd);
    e->id = 0; e->loaded = 0; e->failed = 0;
    return e;
}

/* ── Worker thread: serialized fetch+decode ───────────── */
/* cover_load uses a global stamp counter (cover.c) and is not safe to
   call concurrently; a single worker serializes every load. On success
   the worker publishes EV_COVER_CACHE_LOADED with a CoverCacheResult
   (id + pixel pointer); the main thread consumes it in
   cover_cache_store(). */
static pthread_t       g_worker = 0;
static pthread_mutex_t g_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond   = PTHREAD_COND_INITIALIZER;
static char           *g_queue[COVER_CACHE_MAX * 2];
static size_t          g_qhead = 0, g_qtail = 0, g_qn = 0;
static int             g_stop = 0;

static int queue_push(char *url) {
    int ok = 0;
    pthread_mutex_lock(&g_mutex);
    if (!g_stop && g_qn < sizeof(g_queue)/sizeof(g_queue[0])) {
        g_queue[g_qtail] = url;
        g_qtail = (g_qtail + 1) % (sizeof(g_queue)/sizeof(g_queue[0]));
        g_qn++;
        ok = 1;
    }
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    return ok;
}

static char *queue_pop(void) {
    pthread_mutex_lock(&g_mutex);
    while (g_qn == 0 && !g_stop)
        pthread_cond_wait(&g_cond, &g_mutex);
    char *url = NULL;
    if (g_qn > 0) {
        url = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % (sizeof(g_queue)/sizeof(g_queue[0]));
        g_qn--;
    }
    pthread_mutex_unlock(&g_mutex);
    return url;
}

static void *worker_fn(void *arg) {
    (void)arg;
    for (;;) {
        char *url = queue_pop();
        if (!url) break;
        CoverData cd = {NULL, 0, 0, 0, 0};
        if (cover_load(url, &cd) == 0) {
            CoverCacheResult res;
            res.id = fnv1a64(url);
            res.cd = cd;
            event_bus_publish(EV_COVER_CACHE_LOADED, &res, sizeof(res));
        }
        free(url);
    }
    return NULL;
}

static void ensure_worker(void) {
    if (g_worker) return;
    if (pthread_create(&g_worker, NULL, worker_fn, NULL) != 0) {
        LOG_WARN("cover_cache: worker thread failed");
        g_worker = 0;
    }
}

/* ── API ──────────────────────────────────────────────── */

uint64_t cover_cache_request(const char *url) {
    if (!url || !url[0]) return 0;
    uint64_t id = fnv1a64(url);
    CoverCacheEntry *e = entry_find(id);
    if (e) {
        e->last_use = ++g_clock;
        return id;   /* loaded / failed / in-flight — already tracked */
    }
    char *dup = strdup(url);
    if (!dup) return 0;
    ensure_worker();
    if (!queue_push(dup)) {
        free(dup);
        return 0;
    }
    e = entry_slot();
    e->id = id;
    e->loaded = 0;
    e->failed = 0;
    e->last_use = ++g_clock;
    return id;
}

const CoverData *cover_cache_get(uint64_t id) {
    CoverCacheEntry *e = entry_find(id);
    if (!e) return NULL;
    if (e->loaded) e->last_use = ++g_clock;
    return e->loaded ? &e->cd : NULL;
}

void cover_cache_touch(uint64_t id) {
    CoverCacheEntry *e = entry_find(id);
    if (e) e->last_use = ++g_clock;
}

/* Main thread: consume a worker-delivered load. The payload carries the
   worker-owned pixels; they are moved into the cache and the (empty)
   payload copy is left for the event layer to free. */
void cover_cache_store(const CoverCacheResult *res) {
    if (!res || res->id == 0) return;
    CoverCacheEntry *e = entry_find(res->id);
    if (!e) e = entry_slot();
    e->id = res->id;
    cover_free(&e->cd);
    e->cd = res->cd;
    e->loaded = 1;
    e->failed = 0;
    e->last_use = ++g_clock;
}

void cover_cache_clear(void) {
    pthread_mutex_lock(&g_mutex);
    g_stop = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    if (g_worker) {
        pthread_join(g_worker, NULL);
        g_worker = 0;
    }
    for (size_t i = 0; i < g_n; i++) {
        cover_free(&g_entries[i].cd);
    }
    g_n = 0;
    while (g_qn > 0) {
        free(g_queue[g_qhead]);
        g_qhead = (g_qhead + 1) % (sizeof(g_queue)/sizeof(g_queue[0]));
        g_qn--;
    }
    g_stop = 0;
}
