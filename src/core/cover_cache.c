#include "core/cover_cache.h"
#include "core/cover.h"
#include "core/event_bus.h"
#include "infra/config_paths.h"
#include "infra/log.h"
#include "compat/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* ── Disk cache ─────────────────────────────────────────
   The song list walks rows fast and re-requests the same covers across
   sessions; a download every time (popen curl) is the dominant startup
   cost. Loaded covers are written to ~/.cache/netune/covers/<id>.cov so
   a later run reads them back without the network round-trip.

   File layout: magic "NCOV1" (6B) | width u32 | height u32 | channels
   u32 | RGB/RGBA pixels. Written atomically (tmp + rename) so a crash
   never leaves a half file that would be misread on the next start. */
#define DISK_SUBDIR      "covers"
#define DISK_MAGIC       "NCOV1"
#define DISK_MAGIC_LEN   6

static char g_disk_dir[1024] = {0};
static int  g_disk_ready = 0;

static void disk_init(void) {
    if (g_disk_ready) return;
    g_disk_ready = 1;
    const char *d = netune_xdg_dir("XDG_CACHE_HOME", DISK_SUBDIR);
    snprintf(g_disk_dir, sizeof(g_disk_dir), "%s", d);
    /* netune_ensure_dir mkdir -p's the parent of a file path */
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s" "/x", g_disk_dir);
    netune_ensure_dir(tmp);
}

static void disk_path(uint64_t id, char *out, size_t sz) {
    snprintf(out, sz, "%s" "/%08llx.cov", g_disk_dir,
             (unsigned long long)id);
}

static int disk_write(uint64_t id, const CoverData *cd) {
    if (!g_disk_ready || !cd || !cd->pixels) return -1;
    char path[1200], tmp[1200];
    disk_path(id, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen_utf8(tmp, "wb");
    if (!fp) return -1;
    uint32_t hdr[3];
    hdr[0] = (uint32_t)cd->width;
    hdr[1] = (uint32_t)cd->height;
    hdr[2] = (uint32_t)cd->channels;
    int ok = 1;
    if (fwrite(DISK_MAGIC, 1, DISK_MAGIC_LEN, fp) != DISK_MAGIC_LEN) ok = 0;
    if (ok && fwrite(hdr, sizeof(hdr), 1, fp) != 1) ok = 0;
    size_t px = (size_t)cd->width * cd->height * cd->channels;
    if (ok && px > 0 && fwrite(cd->pixels, 1, px, fp) != px) ok = 0;
    if (ok && fflush(fp) != 0) ok = 0;
    if (fclose(fp) != 0) ok = 0;
    if (ok) {
        if (rename_utf8(tmp, path) != 0) { remove_utf8(tmp); ok = 0; }
    } else {
        remove_utf8(tmp);
    }
    return ok ? 0 : -1;
}

/* Read a cached cover into *out (caller owns, must cover_free later).
   Returns 0 and fills *out on success, -1 on miss/corrupt. */
static int disk_read(uint64_t id, CoverData *out) {
    if (!g_disk_ready || !out) return -1;
    char path[1200];
    disk_path(id, path, sizeof(path));
    FILE *fp = fopen_utf8(path, "rb");
    if (!fp) return -1;
    char magic[DISK_MAGIC_LEN];
    uint32_t hdr[3] = {0, 0, 0};
    int ok = 0;
    if (fread(magic, 1, DISK_MAGIC_LEN, fp) == DISK_MAGIC_LEN &&
        memcmp(magic, DISK_MAGIC, DISK_MAGIC_LEN) == 0 &&
        fread(hdr, sizeof(hdr), 1, fp) == 1) {
        int w = (int)hdr[0], h = (int)hdr[1], ch = (int)hdr[2];
        if (w > 0 && w <= 8192 && h > 0 && h <= 8192 && ch >= 3 && ch <= 4) {
            size_t px = (size_t)w * h * ch;
            uint8_t *pix = (uint8_t*)malloc(px);
            if (pix && fread(pix, 1, px, fp) == px) {
                memset(out, 0, sizeof(*out));
                out->pixels = pix;
                out->width = w;
                out->height = h;
                out->channels = ch;
                out->stamp = cover_stamp_next();
                ok = 1;
            } else {
                free(pix);
            }
        }
    }
    fclose(fp);
    return ok ? 0 : -1;
}

/* ── Worker threads: parallel fetch+decode ──────────────
   Multiple workers drain a shared queue. cover_load_to() is safe to call
   concurrently (its only global, the stamp counter, is mutex-guarded in
   cover.c; stb_image has no global state). On success the worker
   publishes EV_COVER_CACHE_LOADED with a CoverCacheResult (id + pixel
   pointer); the main thread consumes it in cover_cache_store(). */
#define COVER_WORKERS 3

static pthread_t        g_workers[COVER_WORKERS];
static int              g_nworkers = 0;
static pthread_mutex_t  g_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cond   = PTHREAD_COND_INITIALIZER;
static char            *g_queue[COVER_CACHE_MAX * 2];
static size_t           g_qhead = 0, g_qtail = 0, g_qn = 0;
static int              g_stop = 0;

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
        uint64_t id = fnv1a64(url);
        CoverData cd = {NULL, 0, 0, 0, 0};
        /* The song list shows each cover in a square placeholder that is
           ~list_cover_cols x 2 cells. In pixels that is 2*cell_h square.
           Downscale to that display size so the upload stays small — the
           list previously uploaded full-resolution covers (up to 2048px,
           multi-MB base64 each) for every visible row, the dominant
           main-thread cost. */
        int px = 2 * cover_cell_height();
        /* Disk first: a cover cached by an earlier run needs no network. */
        if (disk_read(id, &cd) != 0) {
            if (cover_load_to(url, px, px, &cd) != 0) {
                free(url);
                continue;   /* failed — nothing to publish */
            }
            disk_write(id, &cd);
        }
        CoverCacheResult res;
        res.id = id;
        res.cd = cd;
        event_bus_publish(EV_COVER_CACHE_LOADED, &res, sizeof(res));
        free(url);
    }
    return NULL;
}

static void ensure_workers(void) {
    if (g_nworkers > 0) return;
    for (int i = 0; i < COVER_WORKERS; i++) {
        if (pthread_create(&g_workers[i], NULL, worker_fn, NULL) != 0) {
            LOG_WARN("cover_cache: worker thread %d failed", i);
            break;
        }
        g_nworkers++;
    }
    if (g_nworkers == 0)
        LOG_WARN("cover_cache: no worker threads available");
}

/* ── API ──────────────────────────────────────────────── */

uint64_t cover_cache_request(const char *url) {
    if (!url || !url[0]) return 0;
    disk_init();
    uint64_t id = fnv1a64(url);
    CoverCacheEntry *e = entry_find(id);
    if (e) {
        e->last_use = ++g_clock;
        return id;   /* loaded / failed / in-flight — already tracked */
    }
    /* Reserve the entry BEFORE queueing so a concurrent request for the
       same url in the same frame sees it as in-flight and doesn't enqueue
       a duplicate fetch. */
    e = entry_slot();
    e->id = id;
    e->loaded = 0;
    e->failed = 0;
    e->last_use = ++g_clock;
    char *dup = strdup(url);
    if (!dup) return id;
    ensure_workers();
    if (!queue_push(dup)) {
        free(dup);
    }
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
    for (int i = 0; i < g_nworkers; i++)
        pthread_join(g_workers[i], NULL);
    g_nworkers = 0;
    for (size_t i = 0; i < g_n; i++) {
        cover_free(&g_entries[i].cd);
    }
    g_n = 0;
    while (g_qn > 0) {
        free(g_queue[g_qhead]);
        g_qhead = (g_qhead + 1) % (sizeof(g_queue)/sizeof(g_queue[0]));
        g_qn--;
    }
    pthread_mutex_lock(&g_mutex);
    g_stop = 0;
    pthread_mutex_unlock(&g_mutex);
}
