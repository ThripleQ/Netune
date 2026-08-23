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
    FILE *fp = fopen_utf8(path, "wb");
    if (!fp) { yyjson_mut_doc_free(doc); return -1; }
    int ok = yyjson_mut_write_fp(fp, doc, YYJSON_WRITE_PRETTY, NULL, NULL) != 0;
    fclose(fp);
    yyjson_mut_doc_free(doc);
    return ok ? 0 : -1;
}

/* ── Overrides (preference, persistent) ─────────────── */
static void overrides_path(char *out, size_t sz) { path_for(out, sz, OVR_FILE); }

static yyjson_mut_val *overrides_root(yyjson_mut_doc **doc_out) {
    char path[1100];
    overrides_path(path, sizeof(path));
    yyjson_doc *doc = json_load_file(path);
    yyjson_mut_doc *mdoc;
    yyjson_mut_val *root;
    if (doc) {
        mdoc = yyjson_doc_mut_copy(doc, NULL);
        yyjson_doc_free(doc);
        root = yyjson_mut_doc_get_root(mdoc);
        if (!root) {
            root = yyjson_mut_obj(mdoc);
            yyjson_mut_doc_set_root(mdoc, root);
        }
    } else {
        mdoc = yyjson_mut_doc_new(NULL);
        root = yyjson_mut_obj(mdoc);
        yyjson_mut_doc_set_root(mdoc, root);
    }
    *doc_out = mdoc;
    return root;
}

int nq_override_set(const char *song_id, const char *level) {
    if (!song_id || !*song_id || level_index(level) < 0) return -1;
    yyjson_mut_doc *mdoc;
    yyjson_mut_val *root = overrides_root(&mdoc);
    if (!root) { yyjson_mut_doc_free(mdoc); return -1; }
    yyjson_mut_obj_add_str(mdoc, root, song_id, level);
    char path[1100];
    overrides_path(path, sizeof(path));
    netune_ensure_dir(path);
    int rc = json_save_root(path, root);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

int nq_override_get(const char *song_id, char *level, size_t sz) {
    if (!song_id || !level || sz == 0) return -1;
    char path[1100];
    overrides_path(path, sizeof(path));
    yyjson_doc *doc = json_load_file(path);
    if (!doc) return -1;
    int rc = -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *v = root && yyjson_is_obj(root) ? yyjson_obj_get(root, song_id) : NULL;
    if (v && yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        if (level_index(s) >= 0) {
            snprintf(level, sz, "%s", s);
            rc = 0;
        }
    }
    yyjson_doc_free(doc);
    return rc;
}

int nq_override_del(const char *song_id) {
    if (!song_id || !*song_id) return -1;
    yyjson_mut_doc *mdoc;
    yyjson_mut_val *root = overrides_root(&mdoc);
    if (!root) { yyjson_mut_doc_free(mdoc); return -1; }
    yyjson_mut_obj_remove_key(root, song_id);
    char path[1100];
    overrides_path(path, sizeof(path));
    int rc = json_save_root(path, root);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

/* ── Source-table cache (LRU-capped) ────────────────── */
static void cache_path(char *out, size_t sz) { path_for(out, sz, CACHE_FILE); }

int nq_cache_get(const char *song_id, unsigned *mask_out, int *br_out) {
    if (!song_id || !mask_out) return -1;
    char path[1100];
    cache_path(path, sizeof(path));
    yyjson_doc *doc = json_load_file(path);
    if (!doc) return -1;
    int rc = -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *e = root && yyjson_is_obj(root) ? yyjson_obj_get(root, song_id) : NULL;
    if (e && yyjson_is_obj(e)) {
        yyjson_val *m = yyjson_obj_get(e, "mask");
        if (m && yyjson_is_num(m)) {
            *mask_out = (unsigned)yyjson_get_int(m);
            if (br_out) {
                for (int i = 0; i < NQ_LEVELS; i++) br_out[i] = 0;
                yyjson_val *br = yyjson_obj_get(e, "br");
                if (br && yyjson_is_arr(br)) {
                    size_t n = yyjson_arr_size(br);
                    for (size_t i = 0; i < n && i < (size_t)NQ_LEVELS; i++) {
                        yyjson_val *b = yyjson_arr_get(br, i);
                        if (b && yyjson_is_num(b)) br_out[i] = yyjson_get_int(b);
                    }
                }
            }
            rc = 0;
        }
    }
    yyjson_doc_free(doc);
    return rc;
}

static void cache_prune(yyjson_mut_doc *mdoc, yyjson_mut_val *root) {
    (void)mdoc;
    size_t max = (size_t)CACHE_MAX;
    while (yyjson_mut_obj_size(root) > max) {
        /* drop the entry with the smallest ts (least recently written) */
        const char *oldest_key = NULL;
        size_t oldest_key_len = 0;
        long long oldest = 0;
        int first = 1;
        size_t idx, maxi;
        yyjson_mut_val *k, *v;
        yyjson_mut_obj_foreach(root, idx, maxi, k, v) {
            long long ts = 0;
            if (yyjson_mut_is_obj(v)) {
                yyjson_mut_val *t = yyjson_mut_obj_get(v, "ts");
                if (t && yyjson_mut_is_num(t)) ts = yyjson_mut_get_int(t);
            }
            if (first || ts < oldest) {
                first = 0;
                oldest = ts;
                oldest_key = yyjson_mut_get_str(k);
                oldest_key_len = yyjson_mut_get_len(k);
            }
        }
        if (!oldest_key) break;
        yyjson_mut_obj_remove_keyn(root, oldest_key, oldest_key_len);
    }
}

int nq_cache_put(const char *song_id, unsigned mask, const int *br) {
    if (!song_id || !*song_id) return -1;
    char path[1100];
    cache_path(path, sizeof(path));
    netune_ensure_dir(path);

    yyjson_mut_doc *mdoc;
    yyjson_mut_val *root;
    yyjson_doc *doc = json_load_file(path);
    if (doc) {
        mdoc = yyjson_doc_mut_copy(doc, NULL);
        yyjson_doc_free(doc);
        root = yyjson_mut_doc_get_root(mdoc);
        if (!root) { root = yyjson_mut_obj(mdoc); yyjson_mut_doc_set_root(mdoc, root); }
    } else {
        mdoc = yyjson_mut_doc_new(NULL);
        root = yyjson_mut_obj(mdoc);
        yyjson_mut_doc_set_root(mdoc, root);
    }

    yyjson_mut_val *e = yyjson_mut_obj(mdoc);
    yyjson_mut_obj_add_uint(mdoc, e, "mask", mask);
    yyjson_mut_obj_add_int(mdoc, e, "ts", (long long)time(NULL));
    if (br) {
        yyjson_mut_val *arr = yyjson_mut_arr(mdoc);
        for (int i = 0; i < NQ_LEVELS; i++) yyjson_mut_arr_add_int(mdoc, arr, br[i]);
        yyjson_mut_obj_add_val(mdoc, e, "br", arr);
    }
    yyjson_mut_obj_add_val(mdoc, root, song_id, e);

    cache_prune(mdoc, root);
    int rc = json_save_root(path, root);
    yyjson_mut_doc_free(mdoc);
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
