/* audio_cache.c — index + capacity management for the audio cache.
 *
 * Design mirrors netease_quality.c's in-memory mirror pattern: the index
 * (audio_cache.json) is loaded lazily on first use and kept write-through
 * (every mutation updates memory + flushes to disk), so the hot read path
 * (audio_cache_find) never touches the disk after first load.
 *
 * Invariants:
 *   - index lives in the cache root (<XDG_CACHE_HOME>/netune/audio),
 *     never under config/data.
 *   - cache files are plain audio, named "<song_id>.<ext>".
 *   - capacity (cache.audio_limit_mb, default 2048) is enforced by LRU
 *     eviction (oldest ts first) after each commit.
 */
#include "audio_cache.h"
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
#include <dirent.h>
#include <yyjson.h>

#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

#define AUDIO_SUBDIR  "audio"
#define INDEX_FILE    "audio_cache.json"
#define LIMIT_KEY     "cache.audio_limit_mb"
#define DEFAULT_LIMIT_MB 2048
#define ENABLED_KEY   "cache.audio_enabled"

typedef struct {
    char *id;        /* netease song id */
    char *file;      /* file name within the audio dir */
    long long size;  /* bytes */
    long long ts;    /* last use / last write (unix sec) */
    char *quality;   /* quality level the file was cached at */
    int complete;    /* 1 = whole track, 0 = partial (resumable) */
    CacheSegList segs; /* valid byte regions (partial); sorted, non-overlapping */
} AucEntry;

static AucEntry *g_entries = NULL;
static int    g_count = 0;
static int    g_cap   = 0;
static int    g_loaded = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Path helpers ──────────────────────────────────── */
static const char *audio_dir(void) {
    static char buf[1024];
    static int init = 0;
    if (!init) {
        const char *d = netune_xdg_dir("XDG_CACHE_HOME", NULL);
        snprintf(buf, sizeof(buf), "%s" PATH_SEP "%s", d, AUDIO_SUBDIR);
        init = 1;
    }
    return buf;
}

static void index_path(char *out, size_t sz) {
    snprintf(out, sz, "%s" PATH_SEP "%s", audio_dir(), INDEX_FILE);
}

static void ensure_audio_dir(void) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s" PATH_SEP "x", audio_dir());
    netune_ensure_dir(tmp);  /* mkdir -p the audio dir */
}

static long long file_size(const char *path) {
    struct stat st;
    if (stat_utf8(path, &st) != 0) return 0;
    return (long long)st.st_size;
}

/* basename within the audio dir (handle both separators) */
static const char *dir_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *last = (slash && bslash) ? (slash > bslash ? slash : bslash)
                     : slash ? slash : bslash;
    return last ? last + 1 : path;
}

static int audio_enabled(void) {
    Config *cfg = config_global();
    if (!cfg) return 1;
    return config_get_bool(cfg, ENABLED_KEY, true) ? 1 : 0;
}

/* ── Generic JSON helpers ──────────────────────────── */
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

/* ── Index load / flush (write-through) ────────────── */
static void load_locked(void) {
    if (g_loaded) return;
    g_loaded = 1;
    char path[1100];
    index_path(path, sizeof(path));
    yyjson_doc *doc = json_load_file(path);
    if (!doc) return;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root && yyjson_is_obj(root)) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(root, idx, max, k, v) {
            const char *id = yyjson_get_str(k);
            if (!id || !yyjson_is_obj(v)) continue;
            const char *file = yyjson_get_str(yyjson_obj_get(v, "file"));
            if (!file || !*file) continue;
            if (g_count == g_cap) {
                g_cap = g_cap ? g_cap * 2 : 16;
                g_entries = (AucEntry*)realloc(g_entries,
                        (size_t)g_cap * sizeof(*g_entries));
            }
            AucEntry *e = &g_entries[g_count];
            cache_seglist_init(&e->segs);   /* must init before add */
            e->id = strdup(id);
            e->file = strdup(file);
            yyjson_val *s = yyjson_obj_get(v, "size");
            e->size = s && yyjson_is_num(s) ? (long long)yyjson_get_int(s) : 0;
            yyjson_val *t = yyjson_obj_get(v, "ts");
            e->ts = t && yyjson_is_num(t) ? (long long)yyjson_get_int(t) : 0;
            yyjson_val *q = yyjson_obj_get(v, "quality");
            e->quality = strdup(q && yyjson_is_str(q) ? yyjson_get_str(q) : "");
            yyjson_val *c = yyjson_obj_get(v, "complete");
            e->complete = c && yyjson_is_bool(c)
                ? (yyjson_get_bool(c) ? 1 : 0) : 1;  /* legacy entries = full */
            /* byte regions (range map, M5): ordered segment list. Legacy
               entries store prefix_size/tail_at instead — convert them. */
            yyjson_val *sg = yyjson_obj_get(v, "segments");
            if (sg && yyjson_is_arr(sg)) {
                size_t si, smax;
                yyjson_val *sv;
                yyjson_arr_foreach(sg, si, smax, sv) {
                    yyjson_val *s0 = yyjson_arr_get(sv, 0);
                    yyjson_val *s1 = yyjson_arr_get(sv, 1);
                    if (s0 && s1 && yyjson_is_num(s0) && yyjson_is_num(s1))
                        cache_seglist_add(&e->segs,
                                          (int64_t)yyjson_get_int(s0),
                                          (int64_t)yyjson_get_int(s1));
                }
            } else {
                int64_t legacy_prefix = 0, legacy_tail = 0;
                yyjson_val *ps = yyjson_obj_get(v, "prefix_size");
                legacy_prefix = ps && yyjson_is_num(ps)
                    ? (long long)yyjson_get_int(ps) : 0;
                yyjson_val *ta = yyjson_obj_get(v, "tail_at");
                legacy_tail = ta && yyjson_is_num(ta)
                    ? (long long)yyjson_get_int(ta) : 0;
                if (legacy_prefix > 0)
                    cache_seglist_add(&e->segs, 0, legacy_prefix);
                if (legacy_tail > 0 && legacy_tail < e->size)
                    cache_seglist_add(&e->segs, legacy_tail,
                                      e->size - legacy_tail);
            }
            /* a whole-track entry always reports [0, size) as valid */
            if (e->complete) {
                if (e->segs.count == 0 && e->size > 0)
                    cache_seglist_add(&e->segs, 0, e->size);
            }
            g_count++;
        }
    }
    yyjson_doc_free(doc);
}

static int flush_locked(void) {
    char path[1100];
    index_path(path, sizeof(path));
    ensure_audio_dir();
    yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    for (int i = 0; i < g_count; i++) {
        yyjson_mut_val *e = yyjson_mut_obj(mdoc);
        yyjson_mut_obj_add_str(mdoc, e, "file", g_entries[i].file);
        yyjson_mut_obj_add_int(mdoc, e, "size", (int64_t)g_entries[i].size);
        yyjson_mut_obj_add_int(mdoc, e, "ts", (int64_t)g_entries[i].ts);
        yyjson_mut_obj_add_str(mdoc, e, "quality", g_entries[i].quality);
        yyjson_mut_obj_add_bool(mdoc, e, "complete", g_entries[i].complete ? 1 : 0);
        /* range map: ordered segment array [[start,len],...] */
        yyjson_mut_val *sga = yyjson_mut_arr(mdoc);
        for (int k = 0; k < g_entries[i].segs.count; k++) {
            yyjson_mut_val *pair = yyjson_mut_arr(mdoc);
            yyjson_mut_arr_add_int(mdoc, pair, g_entries[i].segs.segs[k].start);
            yyjson_mut_arr_add_int(mdoc, pair, g_entries[i].segs.segs[k].len);
            yyjson_mut_arr_add_val(sga, pair);
        }
        yyjson_mut_obj_add_val(mdoc, e, "segments", sga);
        /* legacy mirror fields so older builds can still read the regions */
        int64_t legacy_prefix = (g_entries[i].segs.count > 0 &&
                                 g_entries[i].segs.segs[0].start == 0)
            ? g_entries[i].segs.segs[0].len : 0;
        int64_t legacy_tail = 0;
        if (g_entries[i].segs.count >= 2)
            legacy_tail = g_entries[i].segs.segs[g_entries[i].segs.count - 1].start;
        yyjson_mut_obj_add_int(mdoc, e, "prefix_size", legacy_prefix);
        yyjson_mut_obj_add_int(mdoc, e, "tail_at", legacy_tail);
        yyjson_mut_obj_add_val(mdoc, root, g_entries[i].id, e);
    }
    int rc = json_save_root(path, root);
    yyjson_mut_doc_free(mdoc);
    return rc;
}

static int find_locked(const char *song_id) {
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_entries[i].id, song_id) == 0) return i;
    return -1;
}

/* Evict least-recently-used files (oldest ts) until total size fits the
   config cap. Runs on the memory mirror; caller flushes after. */
static void prune_locked(void) {
    Config *cfg = config_global();
    long long limit = (long long)
        (cfg ? config_get_int(cfg, LIMIT_KEY, DEFAULT_LIMIT_MB)
             : DEFAULT_LIMIT_MB) * 1024LL * 1024LL;
    while (g_count > 0) {
        long long total = 0;
        for (int i = 0; i < g_count; i++) total += g_entries[i].size;
        if (total <= limit) break;
        int oldest = 0;
        for (int i = 1; i < g_count; i++)
            if (g_entries[i].ts < g_entries[oldest].ts) oldest = i;
        char p[1100];
        snprintf(p, sizeof(p), "%s" PATH_SEP "%s",
                 audio_dir(), g_entries[oldest].file);
        remove_utf8(p);
        cache_seglist_free(&g_entries[oldest].segs);
        free(g_entries[oldest].id);
        free(g_entries[oldest].file);
        free(g_entries[oldest].quality);
        g_entries[oldest] = g_entries[g_count - 1];
        g_count--;
    }
}

/* ── Public API ────────────────────────────────────── */
int audio_cache_enabled(void) { return audio_enabled(); }

const char *audio_cache_dir(void) { return audio_dir(); }

void audio_cache_ensure_dir(void) { ensure_audio_dir(); }

int audio_cache_find(const char *song_id, const char *quality,
                     char *path_out, size_t sz, int *complete,
                     CacheSegList *segs) {
    if (!song_id || !*song_id || !path_out || sz == 0) return -1;
    if (!audio_enabled()) return -1;
    pthread_mutex_lock(&g_mutex);
    load_locked();
    int rc = -1;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].id, song_id) == 0) {
            /* only reuse the file when the cached quality matches the
               level we are about to play; otherwise re-download */
            if (quality && g_entries[i].quality &&
                strcmp(quality, g_entries[i].quality) == 0) {
                snprintf(path_out, sz, "%s" PATH_SEP "%s",
                         audio_dir(), g_entries[i].file);
                if (complete) *complete = g_entries[i].complete;
                if (segs) {
                    /* Deep copy: the caller owns the returned segment list.
                       A shallow struct copy would hand the caller a pointer
                       into the index's internal heap array — a subsequent
                       caller free()/add() would then corrupt the index
                       (double-free, UAF, or a realloc that leaves the index
                       entry dangling). Drop any pre-initialized backing
                       array first so the caller's init() is not leaked. */
                    cache_seglist_free(segs);
                    cache_seglist_init(segs);
                    for (int k = 0; k < g_entries[i].segs.count; k++)
                        cache_seglist_add(segs,
                                          g_entries[i].segs.segs[k].start,
                                          g_entries[i].segs.segs[k].len);
                }
                rc = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return rc;
}

char *audio_cache_part_path(const char *song_id) {
    if (!audio_enabled() || !song_id || !*song_id) return NULL;
    char *out = (char*)malloc(1100);
    if (!out) return NULL;
    snprintf(out, 1100, "%s" PATH_SEP "%s.part", audio_dir(), song_id);
    return out;
}

char *audio_cache_final_path(const char *song_id, const char *ext) {
    if (!song_id || !*song_id) return NULL;
    const char *e = (ext && ext[0]) ? ext : ".mp3";
    char *out = (char*)malloc(1100);
    if (!out) return NULL;
    snprintf(out, 1100, "%s" PATH_SEP "%s%s", audio_dir(), song_id, e);
    return out;
}

int audio_cache_commit(const char *song_id, const char *final_path,
                       const char *quality, int complete,
                       const CacheSegList *segs) {
    if (!audio_enabled() || !song_id || !*song_id || !final_path) return -1;
    pthread_mutex_lock(&g_mutex);
    load_locked();
    long long sz = file_size(final_path);
    long long ts = (long long)time(NULL);
    int idx = find_locked(song_id);
    if (idx >= 0) {
        /* superseded (e.g. re-cached at another quality): drop the old
           file unless it is the same path we are committing */
        char oldp[1100];
        snprintf(oldp, sizeof(oldp), "%s" PATH_SEP "%s",
                 audio_dir(), g_entries[idx].file);
        if (strcmp(oldp, final_path) != 0) remove_utf8(oldp);
        cache_seglist_free(&g_entries[idx].segs);   /* replace the regions */
        free(g_entries[idx].file);
        free(g_entries[idx].quality);
        g_entries[idx].file = strdup(dir_basename(final_path));
        g_entries[idx].quality = strdup(quality ? quality : "");
        g_entries[idx].size = sz;
        g_entries[idx].ts = ts;
        g_entries[idx].complete = complete ? 1 : 0;
        cache_seglist_init(&g_entries[idx].segs);
        if (complete) {
            if (sz > 0) cache_seglist_add(&g_entries[idx].segs, 0, sz);
        } else if (segs) {
            for (int k = 0; k < segs->count; k++)
                cache_seglist_add(&g_entries[idx].segs,
                                  segs->segs[k].start, segs->segs[k].len);
        }
    } else {
        if (g_count == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 16;
            g_entries = (AucEntry*)realloc(g_entries,
                    (size_t)g_cap * sizeof(*g_entries));
        }
        AucEntry *e = &g_entries[g_count];
        e->id = strdup(song_id);
        e->file = strdup(dir_basename(final_path));
        e->quality = strdup(quality ? quality : "");
        e->size = sz;
        e->ts = ts;
        e->complete = complete ? 1 : 0;
        cache_seglist_init(&e->segs);
        if (complete) {
            if (sz > 0) cache_seglist_add(&e->segs, 0, sz);
        } else if (segs) {
            for (int k = 0; k < segs->count; k++)
                cache_seglist_add(&e->segs, segs->segs[k].start,
                                  segs->segs[k].len);
        }
        g_count++;
    }
    prune_locked();
    int rc = flush_locked();
    pthread_mutex_unlock(&g_mutex);
    return rc;
}

void audio_cache_touch(const char *song_id) {
    if (!song_id || !*song_id) return;
    pthread_mutex_lock(&g_mutex);
    load_locked();
    int idx = find_locked(song_id);
    if (idx >= 0) {
        g_entries[idx].ts = (long long)time(NULL);
        flush_locked();
    }
    pthread_mutex_unlock(&g_mutex);
}

void audio_cache_remove(const char *song_id) {
    if (!song_id || !*song_id) return;
    pthread_mutex_lock(&g_mutex);
    load_locked();
    int idx = find_locked(song_id);
    if (idx >= 0) {
        char p[1100];
        snprintf(p, sizeof(p), "%s" PATH_SEP "%s",
                 audio_dir(), g_entries[idx].file);
        remove_utf8(p);
        cache_seglist_free(&g_entries[idx].segs);
        free(g_entries[idx].id);
        free(g_entries[idx].file);
        free(g_entries[idx].quality);
        g_entries[idx] = g_entries[g_count - 1];
        g_count--;
        flush_locked();
    }
    pthread_mutex_unlock(&g_mutex);
}

int audio_cache_clear(void) {
    pthread_mutex_lock(&g_mutex);
    load_locked();
    int removed = 0;
    for (int i = 0; i < g_count; i++) {
        char p[1100];
        snprintf(p, sizeof(p), "%s" PATH_SEP "%s",
                 audio_dir(), g_entries[i].file);
        if (remove_utf8(p) == 0) removed++;
        cache_seglist_free(&g_entries[i].segs);
        free(g_entries[i].id);
        free(g_entries[i].file);
        free(g_entries[i].quality);
    }
    g_count = 0;
    flush_locked();  /* persist an empty index */
    /* also drop stray .part files left behind by interrupted recordings */
    {
        DIR *d = opendir(audio_dir());
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                const char *n = e->d_name;
                size_t len = strlen(n);
                if (len > 5 && strcmp(n + len - 5, ".part") == 0) {
                    char p[1100];
                    snprintf(p, sizeof(p), "%s" PATH_SEP "%s",
                             audio_dir(), n);
                    if (remove_utf8(p) == 0) removed++;
                }
            }
            closedir(d);
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return removed;
}

long long audio_cache_total_bytes(void) {
    pthread_mutex_lock(&g_mutex);
    load_locked();
    long long total = 0;
    for (int i = 0; i < g_count; i++) total += g_entries[i].size;
    pthread_mutex_unlock(&g_mutex);
    return total;
}

/* Reconcile the index against what is actually on disk, recovering from an
   unclean shutdown (crash/kill). MUST be called while no cache file is being
   actively written (e.g. once at playback startup, before any stream opens):

   - Drops entries whose file is missing (a complete entry whose file was
     lost, or a partial whose file was never created) or is empty/shortened
     below the recorded contiguous prefix (a partial truncated mid-download —
     its gaps are no longer trustworthy).
   - Deletes every leftover .part file. .part files never enter the index
     (they are only promoted to a final entry by commit+rename), so with no
     active downloader any remaining .part is crash residue.

   A complete entry whose file exists is left alone: without knowing the
   format's intrinsic size we cannot verify it, and the worst outcome is a
   decode that stops short (handled as a local playback error), which is
   strictly better than deleting a possibly-good cache entry. Returns 0. */
int audio_cache_reconcile(void) {
    pthread_mutex_lock(&g_mutex);
    load_locked();
    int changed = 0;
    for (int i = g_count - 1; i >= 0; i--) {
        AucEntry *e = &g_entries[i];
        char p[1100];
        snprintf(p, sizeof(p), "%s" PATH_SEP "%s", audio_dir(), e->file);
        struct stat st;
        int drop = 0;
        if (stat_utf8(p, &st) != 0) {
            drop = 1;                       /* entry points at a missing file */
        } else if (st.st_size <= 0) {
            drop = 1;                       /* empty file — unusable */
        } else if (!e->complete) {
            /* a partial whose recorded valid regions extend past the actual
               on-disk size was truncated by a crash mid-download — the
               recorded segments no longer describe the file */
            for (int k = 0; k < e->segs.count; k++) {
                if (e->segs.segs[k].start + e->segs.segs[k].len >
                    (int64_t)st.st_size) {
                    drop = 1;
                    break;
                }
            }
        }
        if (drop) {
            cache_seglist_free(&e->segs);
            free(e->id);
            free(e->file);
            free(e->quality);
            g_entries[i] = g_entries[g_count - 1];
            g_count--;
            changed = 1;
        }
    }
    /* leftover .part files are crash residue (see fn comment) */
    {
        DIR *d = opendir(audio_dir());
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                const char *n = de->d_name;
                size_t len = strlen(n);
                if (len > 5 && strcmp(n + len - 5, ".part") == 0) {
                    char p[1100];
                    snprintf(p, sizeof(p), "%s" PATH_SEP "%s",
                             audio_dir(), n);
                    if (remove_utf8(p) == 0) changed = 1;
                }
            }
            closedir(d);
        }
    }
    if (changed) flush_locked();
    pthread_mutex_unlock(&g_mutex);
    return 0;
}
