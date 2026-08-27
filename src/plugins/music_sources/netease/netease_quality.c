/* netease_quality.c — play-quality resolution + caching for netease source.
 *
 * See netease_quality.h for the design. Key invariants:
 *   - override/global are preferences (kept, in config / overrides file)
 *   - source table is a cache (rebuildable, LRU-capped)
 *   - entitlement (VIP) is NEVER cached — always live
 */
#include "netease_quality.h"
#include "netease_api.h"
#include "infra/config.h"
#include "infra/config_paths.h"
#include "compat/utf8.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <yyjson.h>

#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

/* ── Storage locations ─────────────────────────────── */
#define OVR_FILE   "quality_overrides.json"
#define CACHE_FILE "quality_cache.json"
#define CACHE_MAX  500   /* LRU cap: number of songs */
#define GLOBAL_KEY "netease.quality"
#define DEFAULT_GLOBAL "exhigh"

/* valid quality levels, high→low (index matches NQ_* bits in netease_api.h) */
static const char *const kLevels[] = {
    "jymaster", "sky", "jyeffect", "hires",
    "lossless", "exhigh", "higher", "standard"
};
#define KLEVELS_N (sizeof(kLevels) / sizeof(kLevels[0]))

/* ── Path helpers ──────────────────────────────────── */
static const char *cache_dir(void) {
    static char buf[1024];
    static int init = 0;
    if (!init) {
        const char *d = netune_xdg_dir("XDG_CACHE_HOME", NULL);
        snprintf(buf, sizeof(buf), "%s", d);
        init = 1;
    }
    return buf;
}

static void path_for(char *out, size_t sz, const char *file) {
    snprintf(out, sz, "%s" PATH_SEP "%s", cache_dir(), file);
}

static int level_index(const char *level) {
    if (!level || !*level) return -1;
    for (size_t i = 0; i < KLEVELS_N; i++)
        if (strcmp(kLevels[i], level) == 0) return (int)i;
    return -1;
}

/* ── Generic JSON file load (immutable doc) ─────────── */
static yyjson_doc *json_load_file(const char *path) {
    FILE *fp = fopen_utf8(path, "rb");
    if (!fp) return NULL;
    yyjson_doc *doc = yyjson_read_fp(fp, 0, NULL, NULL);
    fclose(fp);
    return doc;
}

static int json_save_root(const char *path, yyjson_mut_val *root) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, root);
    /* Atomic write (tmp + rename): a direct "wb" overwrite can leave a
       half-written file if the process is killed mid-write, which yyjson
       then fails to parse on the next start (the whole override set would
       be lost). rename() within the same directory is atomic, so the file
       is always either the old complete version or the new complete one. */
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen_utf8(tmp, "wb");
    if (!fp) { yyjson_mut_doc_free(doc); return -1; }
    int ok = yyjson_mut_write_fp(fp, doc, YYJSON_WRITE_PRETTY, NULL, NULL) != 0;
    if (ok) fflush(fp);
    if (fclose(fp) != 0) ok = 0;
    if (ok) {
        if (rename_utf8(tmp, path) != 0) { remove_utf8(tmp); ok = 0; }
    } else {
        remove_utf8(tmp);
    }
    yyjson_mut_doc_free(doc);
    return ok ? 0 : -1;
}

/* ── Overrides (preference, persistent) ───────────────
   In-memory mirror of quality_overrides.json. Loaded lazily on first use
   and kept write-through (set/del update memory + flush to disk), so the
   common read path (nq_override_get) never touches the disk. */

static void overrides_path(char *out, size_t sz) { path_for(out, sz, OVR_FILE); }

/* song_id -> level; linear array is fine (overrides are few, user-set) */
typedef struct { char *id; char *level; } OvrEntry;
static OvrEntry *g_ovr = NULL;
static int   g_ovr_count = 0;
static int   g_ovr_cap   = 0;
static int   g_ovr_loaded = 0;
static pthread_mutex_t g_ovr_mutex = PTHREAD_MUTEX_INITIALIZER;

static void overrides_load_locked(void) {
    if (g_ovr_loaded) return;
    g_ovr_loaded = 1;
    char path[1100];
    overrides_path(path, sizeof(path));
    yyjson_doc *doc = json_load_file(path);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_obj(root)) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(root, idx, max, k, v) {
            const char *id = yyjson_get_str(k);
            const char *lvl = yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
            if (!id || !lvl || level_index(lvl) < 0) continue;
            if (g_ovr_count == g_ovr_cap) {
                g_ovr_cap = g_ovr_cap ? g_ovr_cap * 2 : 8;
                g_ovr = (OvrEntry*)realloc(g_ovr,
                        (size_t)g_ovr_cap * sizeof(*g_ovr));
            }
            g_ovr[g_ovr_count].id    = strdup(id);
            g_ovr[g_ovr_count].level = strdup(lvl);
            g_ovr_count++;
        }
    }
    yyjson_doc_free(doc);
}

static int overrides_flush_locked(void) {
    char path[1100];
    overrides_path(path, sizeof(path));
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    for (int i = 0; i < g_ovr_count; i++)
        yyjson_mut_obj_add_str(mdoc, root, g_ovr[i].id, g_ovr[i].level);
    netune_ensure_dir(path);
    int rc = json_save_root(path, root);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int nq_override_set(const char *song_id, const char *level) {
    if (!song_id || !*song_id || level_index(level) < 0) return -1;
    pthread_mutex_lock(&g_ovr_mutex);
    overrides_load_locked();
    int i;
    for (i = 0; i < g_ovr_count; i++)
        if (strcmp(g_ovr[i].id, song_id) == 0) break;
    if (i < g_ovr_count) {
        free(g_ovr[i].level);
        g_ovr[i].level = strdup(level);
    } else {
        if (g_ovr_count == g_ovr_cap) {
            g_ovr_cap = g_ovr_cap ? g_ovr_cap * 2 : 8;
            g_ovr = (OvrEntry*)realloc(g_ovr,
                    (size_t)g_ovr_cap * sizeof(*g_ovr));
        }
        g_ovr[g_ovr_count].id    = strdup(song_id);
        g_ovr[g_ovr_count].level = strdup(level);
        g_ovr_count++;
    }
    int rc = overrides_flush_locked();
    pthread_mutex_unlock(&g_ovr_mutex);
    return rc;
}

int nq_override_get(const char *song_id, char *level, size_t sz) {
    if (!song_id || !level || sz == 0) return -1;
    pthread_mutex_lock(&g_ovr_mutex);
    overrides_load_locked();
    int rc = -1;
    for (int i = 0; i < g_ovr_count; i++) {
        if (strcmp(g_ovr[i].id, song_id) == 0) {
            snprintf(level, sz, "%s", g_ovr[i].level);
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_ovr_mutex);
    return rc;
}

int nq_override_del(const char *song_id) {
    if (!song_id || !*song_id) return -1;
    pthread_mutex_lock(&g_ovr_mutex);
    overrides_load_locked();
    int rc = -1;
    for (int i = 0; i < g_ovr_count; i++) {
        if (strcmp(g_ovr[i].id, song_id) == 0) {
            free(g_ovr[i].id);
            free(g_ovr[i].level);
            g_ovr[i] = g_ovr[g_ovr_count - 1];
            g_ovr_count--;
            rc = overrides_flush_locked();
            break;
        }
    }
    pthread_mutex_unlock(&g_ovr_mutex);
    return rc;
}

/* ── Source-table cache (LRU-capped) ──────────────────
   In-memory mirror of quality_cache.json: lazily loaded on first use,
   kept write-through (put updates memory + flushes to disk), LRU-capped
   by CACHE_MAX. nq_cache_get never touches the disk after first load. */

static void cache_path(char *out, size_t sz) { path_for(out, sz, CACHE_FILE); }

typedef struct { char *id; unsigned mask; long long ts; int br[NQ_LEVELS]; } CacheEntry;
static CacheEntry *g_cache = NULL;
static int   g_cache_count = 0;
static int   g_cache_cap   = 0;
static int   g_cache_loaded = 0;
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void cache_load_locked(void) {
    if (g_cache_loaded) return;
    g_cache_loaded = 1;
    char path[1100];
    cache_path(path, sizeof(path));
    yyjson_doc *doc = json_load_file(path);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_obj(root)) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(root, idx, max, k, v) {
            const char *id = yyjson_get_str(k);
            if (!id || !yyjson_is_obj(v)) continue;
            yyjson_val *m = yyjson_obj_get(v, "mask");
            if (!m || !yyjson_is_num(m)) continue;
            if (g_cache_count == g_cache_cap) {
                g_cache_cap = g_cache_cap ? g_cache_cap * 2 : 32;
                g_cache = (CacheEntry*)realloc(g_cache,
                        (size_t)g_cache_cap * sizeof(*g_cache));
            }
            CacheEntry *e = &g_cache[g_cache_count];
            e->id   = strdup(id);
            e->mask = (unsigned)yyjson_get_int(m);
            yyjson_val *t = yyjson_obj_get(v, "ts");
            e->ts = t && yyjson_is_num(t) ? (long long)yyjson_get_int(t) : 0;
            for (int i = 0; i < NQ_LEVELS; i++) e->br[i] = 0;
            yyjson_val *br = yyjson_obj_get(v, "br");
            if (br && yyjson_is_arr(br)) {
                size_t n = yyjson_arr_size(br);
                for (size_t i = 0; i < n && i < (size_t)NQ_LEVELS; i++) {
                    yyjson_val *b = yyjson_arr_get(br, i);
                    if (b && yyjson_is_num(b)) e->br[i] = yyjson_get_int(b);
                }
            }
            g_cache_count++;
        }
    }
    yyjson_doc_free(doc);
}

static int cache_find_locked(const char *song_id) {
    for (int i = 0; i < g_cache_count; i++)
        if (strcmp(g_cache[i].id, song_id) == 0) return i;
    return -1;
}

/* drop the entry with the smallest ts (least recently written) until
   under the cap */
static void cache_prune_locked(void) {
    while (g_cache_count > CACHE_MAX) {
        int oldest = 0;
        for (int i = 1; i < g_cache_count; i++)
            if (g_cache[i].ts < g_cache[oldest].ts) oldest = i;
        free(g_cache[oldest].id);
        g_cache[oldest] = g_cache[g_cache_count - 1];
        g_cache_count--;
    }
}

static int cache_flush_locked(void) {
    char path[1100];
    cache_path(path, sizeof(path));
    netune_ensure_dir(path);
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    for (int i = 0; i < g_cache_count; i++) {
        yyjson_mut_val *e = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_uint(mdoc, e, "mask", g_cache[i].mask);
        yyjson_mut_obj_add_int(mdoc, e, "ts", g_cache[i].ts);
        yyjson_mut_val *arr = yyjson_mut_arr(mdoc);
        for (int j = 0; j < NQ_LEVELS; j++)
            yyjson_mut_arr_add_int(mdoc, arr, g_cache[i].br[j]);
        yyjson_mut_obj_add_val(mdoc, e, "br", arr);
        yyjson_mut_obj_add_val(mdoc, root, g_cache[i].id, e);
    }
    int rc = json_save_root(path, root);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int nq_cache_get(const char *song_id, unsigned *mask_out, int *br_out) {
    if (!song_id || !mask_out) return -1;
    pthread_mutex_lock(&g_cache_mutex);
    cache_load_locked();
    int idx = cache_find_locked(song_id);
    int rc = -1;
    if (idx >= 0) {
        *mask_out = g_cache[idx].mask;
        if (br_out) {
            for (int i = 0; i < NQ_LEVELS; i++) br_out[i] = g_cache[idx].br[i];
        }
        rc = 0;
    }
    pthread_mutex_unlock(&g_cache_mutex);
    return rc;
}

int nq_cache_put(const char *song_id, unsigned mask, const int *br) {
    if (!song_id || !*song_id) return -1;
    pthread_mutex_lock(&g_cache_mutex);
    cache_load_locked();
    int idx = cache_find_locked(song_id);
    if (idx >= 0) {
        g_cache[idx].mask = mask;
        if (br) for (int i = 0; i < NQ_LEVELS; i++) g_cache[idx].br[i] = br[i];
        g_cache[idx].ts = (long long)time(NULL);
    } else {
        if (g_cache_count == g_cache_cap) {
            g_cache_cap = g_cache_cap ? g_cache_cap * 2 : 32;
            g_cache = (CacheEntry*)realloc(g_cache,
                    (size_t)g_cache_cap * sizeof(*g_cache));
        }
        CacheEntry *e = &g_cache[g_cache_count];
        e->id   = strdup(song_id);
        e->mask = mask;
        e->ts   = (long long)time(NULL);
        if (br) for (int i = 0; i < NQ_LEVELS; i++) e->br[i] = br[i];
        else    for (int i = 0; i < NQ_LEVELS; i++) e->br[i] = 0;
        g_cache_count++;
    }
    cache_prune_locked();
    int rc = cache_flush_locked();
    pthread_mutex_unlock(&g_cache_mutex);
    return rc;
}

/* ── Global quality (config.json) ───────────────────── */
const char *nq_global_level(void) {
    Config *cfg = config_global();
    const char *v = cfg ? config_get_str(cfg, GLOBAL_KEY, DEFAULT_GLOBAL) : DEFAULT_GLOBAL;
    if (level_index(v) < 0) v = DEFAULT_GLOBAL;
    return v;
}

int nq_global_set(const char *level) {
    if (level_index(level) < 0) return -1;
    Config *cfg = config_global();
    if (!cfg) return -1;
    if (!config_set_str(cfg, GLOBAL_KEY, level)) return -1;
    return config_save(cfg) ? 0 : -1;
}

/* ── Resolution ─────────────────────────────────────── */
char *nq_resolve_level(const char *song_id) {
    if (!song_id || !*song_id) return NULL;

    /* 1. per-song override */
    char lvl[32];
    int use_override = nq_override_get(song_id, lvl, sizeof(lvl)) == 0;

    const char *want = use_override ? lvl : nq_global_level();
    int want_idx = level_index(want);
    if (want_idx < 0) want_idx = 5;  /* exhigh */

    /* 2. source table (cached; probe + cache on miss) */
    unsigned mask = 0;
    int br[NQ_LEVELS];
    int have = nq_cache_get(song_id, &mask, br) == 0;
    if (!have) {
        if (netease_song_music_quality(song_id, &mask, br) == 0) {
            nq_cache_put(song_id, mask, br);
            have = 1;
        }
    }

    /* 3. verify the wanted tier actually has a source; otherwise degrade
       down the ladder (high→low). If nothing usable, return NULL. */
    if (have && mask != 0) {
        for (int i = want_idx; i < (int)KLEVELS_N; i++) {
            if (mask & (1u << (unsigned)i)) {
                return strdup(kLevels[i]);
            }
        }
        return NULL;  /* song exists but none of the tiers we accept */
    }

    /* probe failed or no source data — trust the request */
    return strdup(kLevels[want_idx]);
}
