#include "cache_manager.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <yyjson.h>

/* ── Internals ──────────────────────────────────────── */
#define MAX_CACHE_ENTRIES 8192
#define CACHE_FILE_NAME "cache.json"

static char  g_cache_dir[1024] = {0};
static char  g_cache_path[1100] = {0};

/* ── Helpers ────────────────────────────────────────── */
static void ensure_dir(const char *dir) {
    struct stat st = {0};
    if (stat(dir, &st) == -1) {
        mkdir(dir, 0755);
    }
}

/* ── Init ────────────────────────────────────────────── */
int cache_init(const char *cache_dir) {
    if (!cache_dir) return -1;
    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s", cache_dir);
    snprintf(g_cache_path, sizeof(g_cache_path), "%s/%s", cache_dir, CACHE_FILE_NAME);
    ensure_dir(cache_dir);
    cache_cleanup();
    LOG_INFO("Cache initialized: %s", g_cache_path);
    return 0;
}

/* ── JSON load / save ───────────────────────────────── */
static yyjson_mut_doc* load_doc(void) {
    FILE *fp = fopen(g_cache_path, "rb");
    if (!fp) return NULL;
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_fp(fp, 0, NULL, &err);
    fclose(fp);
    if (!doc) return NULL;
    yyjson_mut_doc *mdoc = yyjson_doc_mut_copy(doc, NULL);
    yyjson_doc_free(doc);
    return mdoc;
}

static int save_doc(yyjson_mut_doc *doc) {
    yyjson_write_err err;
    FILE *fp = fopen(g_cache_path, "wb");
    if (!fp) return -1;
    bool ok = yyjson_mut_write_fp(fp, doc, YYJSON_WRITE_PRETTY, NULL, &err);
    fclose(fp);
    return ok ? 0 : -1;
}

/* ── Cache entry helper ──────────────────────────────── */
static const char* song_key(const char *source, const char *song_id) {
    static char key[512];
    snprintf(key, sizeof(key), "song:%s:%s", source ? source : "", song_id ? song_id : "");
    return key;
}

/* ── put / get song ─────────────────────────────────── */
int cache_put_song(const char *source, const char *song_id,
                   const SongInfo *info, int ttl_sec) {
    if (!source || !song_id || !info) return -1;

    yyjson_mut_doc *doc = load_doc();
    yyjson_mut_val *root;
    if (!doc) {
        doc = yyjson_mut_doc_new(NULL);
        root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
    } else {
        root = yyjson_mut_doc_get_root(doc);
    }

    const char *key = song_key(source, song_id);
    yyjson_mut_val *entry = yyjson_mut_obj(doc);

    time_t expires = ttl_sec > 0 ? time(NULL) + ttl_sec : 0;
    char exp_buf[32];
    snprintf(exp_buf, sizeof(exp_buf), "%ld", (long)expires);

    yyjson_mut_obj_add_str(doc, entry, "expires", exp_buf);
    yyjson_mut_obj_add_str(doc, entry, "title",   info->title ? info->title : "");
    yyjson_mut_obj_add_str(doc, entry, "artist",  info->artist ? info->artist : "");
    yyjson_mut_obj_add_str(doc, entry, "album",   info->album ? info->album : "");
    yyjson_mut_obj_add_int(doc, entry, "duration", info->duration_sec);

    yyjson_mut_obj_add_val(doc, root, key, entry);

    int rc = save_doc(doc);
    yyjson_mut_doc_free(doc);
    return rc;
}

int cache_get_song(const char *source, const char *song_id, SongInfo *out) {
    if (!source || !song_id || !out) return -1;

    yyjson_mut_doc *doc = load_doc();
    if (!doc) return -1;

    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) { yyjson_mut_doc_free(doc); return -1; }

    const char *key = song_key(source, song_id);
    yyjson_mut_val *entry = yyjson_mut_obj_get(root, key);
    if (!entry) { yyjson_mut_doc_free(doc); return -1; }

    /* Check expiry */
    yyjson_mut_val *exp_v = yyjson_mut_obj_get(entry, "expires");
    if (exp_v) {
        long exp = atol(yyjson_mut_get_str(exp_v));
        if (exp > 0 && time(NULL) >= exp) {
            yyjson_mut_doc_free(doc);
            return -1; /* expired */
        }
    }

    memset(out, 0, sizeof(*out));
    out->id     = strdup(song_id);
    out->source = strdup(source);

    yyjson_mut_val *v;
    v = yyjson_mut_obj_get(entry, "title");   if (v) out->title   = strdup(yyjson_mut_get_str(v));
    v = yyjson_mut_obj_get(entry, "artist");  if (v) out->artist  = strdup(yyjson_mut_get_str(v));
    v = yyjson_mut_obj_get(entry, "album");   if (v) out->album   = strdup(yyjson_mut_get_str(v));
    v = yyjson_mut_obj_get(entry, "duration"); if (v) out->duration_sec = yyjson_mut_get_int(v);

    yyjson_mut_doc_free(doc);
    return 0;
}

/* ── Search cache ────────────────────────────────────── */
int cache_search(const char *keyword, int limit, SearchResult *out) {
    if (!keyword || !out) return -1;
    memset(out, 0, sizeof(*out));

    yyjson_mut_doc *doc = load_doc();
    if (!doc) return 0;

    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root || !yyjson_mut_is_obj(root)) { yyjson_mut_doc_free(doc); return 0; }

    /* Iterate over all keys, find song entries matching keyword */
    size_t idx, max;
    yyjson_mut_val *k, *v;
    int max_results = limit > 0 ? limit : 50;
    int count = 0;
    SongInfo *results = calloc((size_t)max_results, sizeof(SongInfo));
    if (!results) { yyjson_mut_doc_free(doc); return -1; }

    yyjson_mut_obj_foreach(root, idx, max, k, v) {
        const char *key_str = yyjson_mut_get_str(k);
        if (!key_str || strncmp(key_str, "song:", 5) != 0) continue;
        if (count >= max_results) break;

        /* Check expiry */
        yyjson_mut_val *exp_v = yyjson_mut_obj_get(v, "expires");
        if (exp_v) {
            long exp = atol(yyjson_mut_get_str(exp_v));
            if (exp > 0 && time(NULL) >= exp) continue;
        }

        /* Check keyword match */
        yyjson_mut_val *title  = yyjson_mut_obj_get(v, "title");
        yyjson_mut_val *artist = yyjson_mut_obj_get(v, "artist");
        yyjson_mut_val *album  = yyjson_mut_obj_get(v, "album");

        const char *t = title  ? yyjson_mut_get_str(title) : "";
        const char *a = artist ? yyjson_mut_get_str(artist) : "";
        const char *al = album ? yyjson_mut_get_str(album) : "";

        if (strcasestr(t, keyword) || strcasestr(a, keyword) || strcasestr(al, keyword)) {
            SongInfo *si = &results[count];
            si->id     = strdup(key_str + 5); /* skip "song:" prefix */
            si->source = strdup("local");     /* simplified */
            si->title  = strdup(t);
            si->artist = strdup(a);
            si->album  = strdup(al);
            v = yyjson_mut_obj_get(v, "duration");
            if (v) si->duration_sec = yyjson_mut_get_int(v);
            count++;
        }
    }

    out->songs = results;
    out->count = count;
    out->total = count;

    yyjson_mut_doc_free(doc);
    return 0;
}

/* ── Generic put / get ──────────────────────────────── */
int cache_put(const char *key, const char *json_value, int ttl_sec) {
    if (!key || !json_value) return -1;

    yyjson_mut_doc *doc = load_doc();
    yyjson_mut_val *root;
    if (!doc) {
        doc = yyjson_mut_doc_new(NULL);
        root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
    } else {
        root = yyjson_mut_doc_get_root(doc);
    }

    yyjson_mut_val *entry = yyjson_mut_obj(doc);
    time_t expires = ttl_sec > 0 ? time(NULL) + ttl_sec : 0;
    char exp_buf[32];
    snprintf(exp_buf, sizeof(exp_buf), "%ld", (long)expires);
    yyjson_mut_obj_add_str(doc, entry, "expires", exp_buf);
    yyjson_mut_obj_add_str(doc, entry, "data", json_value);

    yyjson_mut_obj_add_val(doc, root, key, entry);
    int rc = save_doc(doc);
    yyjson_mut_doc_free(doc);
    return rc;
}

int cache_get(const char *key, char *buf, size_t buf_size) {
    if (!key || !buf || buf_size == 0) return -1;

    yyjson_mut_doc *doc = load_doc();
    if (!doc) return -1;

    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) { yyjson_mut_doc_free(doc); return -1; }

    yyjson_mut_val *entry = yyjson_mut_obj_get(root, key);
    if (!entry) { yyjson_mut_doc_free(doc); return -1; }

    yyjson_mut_val *exp_v = yyjson_mut_obj_get(entry, "expires");
    if (exp_v) {
        long exp = atol(yyjson_mut_get_str(exp_v));
        if (exp > 0 && time(NULL) >= exp) {
            yyjson_mut_doc_free(doc);
            return -1;
        }
    }

    yyjson_mut_val *data_v = yyjson_mut_obj_get(entry, "data");
    if (!data_v) { yyjson_mut_doc_free(doc); return -1; }

    const char *val = yyjson_mut_get_str(data_v);
    size_t len = strlen(val);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, val, len);
    buf[len] = '\0';

    yyjson_mut_doc_free(doc);
    return (int)len;
}

/* ── Maintenance ─────────────────────────────────────── */
void cache_cleanup(void) {
    yyjson_mut_doc *doc = load_doc();
    if (!doc) return;

    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root || !yyjson_mut_is_obj(root)) {
        yyjson_mut_doc_free(doc);
        return;
    }

    /* Collect expired keys */
    const char *expired_keys[256];
    int n_expired = 0;
    size_t idx, max;
    yyjson_mut_val *k, *v;

    yyjson_mut_obj_foreach(root, idx, max, k, v) {
        yyjson_mut_val *exp_v = yyjson_mut_obj_get(v, "expires");
        if (exp_v) {
            long exp = atol(yyjson_mut_get_str(exp_v));
            if (exp > 0 && time(NULL) >= exp) {
                if (n_expired < 256)
                    expired_keys[n_expired++] = yyjson_mut_get_str(k);
            }
        }
    }

    /* Remove expired entries */
    for (int i = 0; i < n_expired; i++)
        yyjson_mut_obj_remove_key(root, expired_keys[i]);

    if (n_expired > 0) {
        save_doc(doc);
        LOG_INFO("Cache cleanup: removed %d expired entries", n_expired);
    }

    yyjson_mut_doc_free(doc);
}

void cache_clear(void) {
    remove(g_cache_path);
    LOG_INFO("Cache cleared");
}

void cache_shutdown(void) {
    LOG_INFO("Cache shutdown");
}
