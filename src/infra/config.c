#include "config.h"
#include "log.h"
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "compat/utf8.h"



/* ── Global singleton ──────────────────────────────── */
static Config *g_config = NULL;

void config_set_global(Config *cfg) { g_config = cfg; }
Config* config_global(void) { return g_config; }

struct Config {
    yyjson_doc  *doc;
    yyjson_val  *root;
    yyjson_mut_doc *mdoc;   /* mutable copy, created lazily for writes */
    char        *path;      /* file this config was loaded from */
    /* owned key strings added to the mutable tree. yyjson stores mut
       object keys by reference — the caller must keep them alive for
       the lifetime of the document. */
    char       **keys;
    size_t       key_count;
    size_t       key_cap;
    /* Guards doc/mdoc/keys. Reads and writes arrive from the UI thread
       (volume, settings) and worker threads (download dirs, quality),
       and yyjson mutation is not thread-safe. */
    pthread_mutex_t lock;
};

Config* config_load(const char *path) {
    FILE *fp = fopen_utf8(path, "rb");
    if (!fp) {
        LOG_WARN("Cannot open config file: %s", path);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        LOG_ERROR("Cannot seek config file: %s", path);
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        LOG_ERROR("Cannot size config file: %s", path);
        fclose(fp);
        return NULL;
    }
    /* Refuse absurd sizes instead of attempting a near-SIZE_MAX read. */
    if (size > 16 * 1024 * 1024) {
        LOG_ERROR("Config file too large: %s (%ld bytes)", path, size);
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);

    char *buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        LOG_ERROR("Short read on config file: %s (%zu/%ld)", path, got, size);
        free(buf);
        return NULL;
    }
    buf[size] = '\0';

    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts(buf, (size_t)size, YYJSON_READ_ALLOW_COMMENTS, NULL, &err);
    free(buf);

    if (!doc) {
        LOG_ERROR("JSON parse error at pos %zu: %s", (size_t)err.pos, err.msg);
        return NULL;
    }

    Config *cfg = (Config*)calloc(1, sizeof(Config));
    if (!cfg) { yyjson_doc_free(doc); return NULL; }
    cfg->doc  = doc;
    cfg->root = yyjson_doc_get_root(doc);
    cfg->path = strdup(path);
    if (!cfg->path) {
        yyjson_doc_free(doc);
        free(cfg);
        return NULL;
    }
    pthread_mutex_init(&cfg->lock, NULL);

    LOG_INFO("Config loaded: %s", path);
    return cfg;
}

void config_free(Config *cfg) {
    if (!cfg) return;
    if (cfg->mdoc) yyjson_mut_doc_free(cfg->mdoc);
    for (size_t i = 0; i < cfg->key_count; i++) free(cfg->keys[i]);
    free(cfg->keys);
    free(cfg->path);
    yyjson_doc_free(cfg->doc);
    pthread_mutex_destroy(&cfg->lock);
    free(cfg);
}

/* resolve dotted key path: "a.b.c" -> obj["a"]["b"]["c"] */
static yyjson_val* resolve(Config *cfg, const char *key) {
    if (!cfg) return NULL;
    if (!cfg->root) return NULL;
    yyjson_val *v = cfg->root;

    char *k = strdup(key);
    if (!k) return NULL;
    char *save = NULL;
    char *tok = strtok_r(k, ".", &save);
    while (tok && v) {
        /* check for array index: key[idx] */
        int idx = -1;
        char *bracket = strchr(tok, '[');
        if (bracket) {
            *bracket = '\0';
            idx = atoi(bracket + 1);
        }

        if (yyjson_is_obj(v)) {
            v = yyjson_obj_get(v, tok);
        } else {
            v = NULL;
        }

        /* if array index, index into array */
        if (v && idx >= 0 && yyjson_is_arr(v))
            v = yyjson_arr_get(v, (size_t)idx);

        tok = strtok_r(NULL, ".", &save);
    }
    free(k);
    return v;
}

/* resolve dotted key path in the mutable tree (read-only, no creation) */
static yyjson_mut_val* resolve_mut_read(Config *cfg, const char *key) {
    if (!cfg->mdoc) return NULL;
    yyjson_mut_val *v = yyjson_mut_doc_get_root(cfg->mdoc);
    char *k = strdup(key);
    if (!k) return NULL;
    char *save = NULL;
    char *tok = strtok_r(k, ".", &save);
    while (tok && v) {
        int idx = -1;
        char *bracket = strchr(tok, '[');
        if (bracket) {
            *bracket = '\0';
            idx = atoi(bracket + 1);
        }
        v = yyjson_mut_is_obj(v) ? yyjson_mut_obj_get(v, tok) : NULL;
        if (v && idx >= 0 && yyjson_mut_is_arr(v))
            v = yyjson_mut_arr_get(v, (size_t)idx);
        tok = strtok_r(NULL, ".", &save);
    }
    free(k);
    return v;
}

const char* config_get_str(Config *cfg, const char *key, const char *fallback) {
    if (!cfg) return fallback;
    pthread_mutex_lock(&cfg->lock);
    const char *out = fallback;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_str(mv)) {
        out = yyjson_mut_get_str(mv);
    } else {
        yyjson_val *v = resolve(cfg, key);
        if (v && yyjson_is_str(v)) out = yyjson_get_str(v);
    }
    pthread_mutex_unlock(&cfg->lock);
    return out;
}

int config_get_int(Config *cfg, const char *key, int fallback) {
    if (!cfg) return fallback;
    pthread_mutex_lock(&cfg->lock);
    int out = fallback;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_int(mv)) {
        out = (int)yyjson_mut_get_int(mv);
    } else {
        yyjson_val *v = resolve(cfg, key);
        if (v && yyjson_is_int(v)) out = (int)yyjson_get_int(v);
    }
    pthread_mutex_unlock(&cfg->lock);
    return out;
}

bool config_get_bool(Config *cfg, const char *key, bool fallback) {
    if (!cfg) return fallback;
    pthread_mutex_lock(&cfg->lock);
    bool out = fallback;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_bool(mv)) {
        out = yyjson_mut_get_bool(mv);
    } else {
        yyjson_val *v = resolve(cfg, key);
        if (v && yyjson_is_bool(v)) out = yyjson_get_bool(v);
    }
    pthread_mutex_unlock(&cfg->lock);
    return out;
}

double config_get_double(Config *cfg, const char *key, double fallback) {
    if (!cfg) return fallback;
    pthread_mutex_lock(&cfg->lock);
    double out = fallback;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_num(mv)) {
        out = yyjson_mut_get_num(mv);
    } else {
        yyjson_val *v = resolve(cfg, key);
        if (v && yyjson_is_num(v)) out = yyjson_get_num(v);
    }
    pthread_mutex_unlock(&cfg->lock);
    return out;
}

int config_get_array_size(Config *cfg, const char *key) {
    if (!cfg) return 0;
    pthread_mutex_lock(&cfg->lock);
    int out = 0;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_arr(mv)) {
        out = (int)yyjson_mut_arr_size(mv);
    } else {
        yyjson_val *v = resolve(cfg, key);
        if (v && yyjson_is_arr(v)) out = (int)yyjson_arr_size(v);
    }
    pthread_mutex_unlock(&cfg->lock);
    return out;
}

bool config_has(Config *cfg, const char *key) {
    if (!cfg || !key) return false;
    pthread_mutex_lock(&cfg->lock);
    bool out = resolve(cfg, key) != NULL;
    pthread_mutex_unlock(&cfg->lock);
    return out;
}

/* ── Mutable access (set + persist) ─────────────────── */

/* make an owned copy of a key that will be handed to the mutable tree */
static const char* config_own_key(Config *cfg, const char *key) {
    if (!key) return NULL;
    if (cfg->key_count == cfg->key_cap) {
        size_t ncap = cfg->key_cap ? cfg->key_cap * 2 : 8;
        char **nk = (char**)realloc(cfg->keys, ncap * sizeof(char*));
        if (!nk) return NULL;
        cfg->keys = nk;
        cfg->key_cap = ncap;
    }
    char *dup = strdup(key);
    if (!dup) return NULL;
    cfg->keys[cfg->key_count++] = dup;
    return dup;
}

/* ensure the mutable copy of the doc exists */
static yyjson_mut_doc* config_mut_doc(Config *cfg) {
    if (!cfg) return NULL;
    if (!cfg->mdoc) {
        cfg->mdoc = yyjson_doc_mut_copy(cfg->doc, NULL);
        if (!cfg->mdoc) LOG_ERROR("Failed to create mutable config copy");
    }
    return cfg->mdoc;
}

/* resolve dotted key path inside a mutable object tree, creating any
   missing intermediate objects on the way. Returns the parent object of
   the leaf key, and stores the leaf value (may be NULL if absent) in
   *out_leaf. Returns NULL on failure (bad path / not an object / OOM).
   Newly added keys are owned by cfg (see config_own_key). */
static yyjson_mut_val* resolve_mut_path(Config *cfg, yyjson_mut_doc *mdoc,
                                        yyjson_mut_val *root,
                                        const char *key,
                                        const char **out_leaf_key,
                                        yyjson_mut_val **out_leaf) {
    char *k = strdup(key);
    if (!k) return NULL;

    yyjson_mut_val *v = root;
    char *save = NULL;
    char *tok = strtok_r(k, ".", &save);
    char *prev = NULL;
    while (tok) {
        if (!yyjson_mut_is_obj(v)) { free(k); return NULL; }
        yyjson_mut_val *next = yyjson_mut_obj_get(v, tok);
        if (!next) {
            const char *owned = config_own_key(cfg, tok);
            if (!owned) { free(k); return NULL; }
            next = yyjson_mut_obj(mdoc);
            if (!next || !yyjson_mut_obj_add_val(mdoc, v, owned, next)) {
                free(k);
                return NULL;
            }
        }
        prev = tok;
        tok = strtok_r(NULL, ".", &save);
        if (tok) {
            v = next;
        } else {
            *out_leaf_key = prev;
            *out_leaf     = next;
            free(k);
            return v;
        }
    }
    free(k);
    return NULL;
}

bool config_set_int(Config *cfg, const char *key, int value) {
    if (!cfg) return false;
    pthread_mutex_lock(&cfg->lock);
    bool ok = false;
    do {
        yyjson_mut_doc *mdoc = config_mut_doc(cfg);
        if (!mdoc) break;
        yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
        if (!root) break;

        const char *leaf_key = NULL;
        yyjson_mut_val *leaf = NULL;
        yyjson_mut_val *parent = resolve_mut_path(cfg, mdoc, root, key,
                                                  &leaf_key, &leaf);
        if (!parent || !leaf_key) break;

        yyjson_mut_set_int(leaf, value);
        ok = true;
    } while (0);
    pthread_mutex_unlock(&cfg->lock);
    return ok;
}

bool config_set_bool(Config *cfg, const char *key, bool value) {
    if (!cfg) return false;
    pthread_mutex_lock(&cfg->lock);
    bool ok = false;
    do {
        yyjson_mut_doc *mdoc = config_mut_doc(cfg);
        if (!mdoc) break;
        yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
        if (!root) break;

        const char *leaf_key = NULL;
        yyjson_mut_val *leaf = NULL;
        yyjson_mut_val *parent = resolve_mut_path(cfg, mdoc, root, key,
                                                  &leaf_key, &leaf);
        if (!parent || !leaf_key) break;

        yyjson_mut_set_bool(leaf, value);
        ok = true;
    } while (0);
    pthread_mutex_unlock(&cfg->lock);
    return ok;
}

bool config_set_str(Config *cfg, const char *key, const char *value) {
    if (!cfg) return false;
    pthread_mutex_lock(&cfg->lock);
    bool ok = false;
    do {
        yyjson_mut_doc *mdoc = config_mut_doc(cfg);
        if (!mdoc) break;
        yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
        if (!root) break;

        const char *leaf_key = NULL;
        yyjson_mut_val *leaf = NULL;
        yyjson_mut_val *parent = resolve_mut_path(cfg, mdoc, root, key,
                                                  &leaf_key, &leaf);
        if (!parent || !leaf_key) break;

        /* yyjson_mut_set_str only stores the pointer without copying.  Callers
           may pass transient buffers (std::string::c_str() etc), so copy the
           string into the doc arena instead of keeping a dangling reference.
           The leaf node keeps its own tag/next chain — only payload (tag+uni)
           is replaced, since the string data must outlive the caller's buffer. */
        size_t len = value ? strlen(value) : 0;
        yyjson_mut_val *nv = yyjson_mut_strncpy(mdoc, value, len);
        if (!nv) break;
        leaf->tag = nv->tag;
        leaf->uni = nv->uni;
        ok = true;
    } while (0);
    pthread_mutex_unlock(&cfg->lock);
    return ok;
}

/* ── Array access ───────────────────────────────────── */

/* Resolve a dotted key to a mutable array, creating any missing
   intermediate objects and the array itself on the way. */
static yyjson_mut_val* resolve_mut_arr(Config *cfg, yyjson_mut_doc *mdoc,
                                       const char *key) {
    if (!cfg || !mdoc || !key) return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
    if (!root) return NULL;

    char *k = strdup(key);
    if (!k) return NULL;
    yyjson_mut_val *v = root;
    char *save = NULL;
    char *tok = strtok_r(k, ".", &save);
    while (tok) {
        char *next = strtok_r(NULL, ".", &save);
        if (!yyjson_mut_is_obj(v)) { free(k); return NULL; }
        yyjson_mut_val *node = yyjson_mut_obj_get(v, tok);
        if (!node) {
            const char *owned = config_own_key(cfg, tok);
            if (!owned) { free(k); return NULL; }
            node = next ? (yyjson_mut_val*)yyjson_mut_obj(mdoc)
                        : (yyjson_mut_val*)yyjson_mut_arr(mdoc);
            if (!node ||
                !yyjson_mut_obj_add_val(mdoc, v, owned, node)) {
                free(k);
                return NULL;
            }
        }
        v = node;
        tok = next;
    }
    free(k);
    return yyjson_mut_is_arr(v) ? v : NULL;
}

bool config_array_push_str(Config *cfg, const char *key, const char *value) {
    if (!cfg || !value) return false;
    pthread_mutex_lock(&cfg->lock);
    bool ok = false;
    do {
        yyjson_mut_doc *mdoc = config_mut_doc(cfg);
        if (!mdoc) break;
        yyjson_mut_val *arr = resolve_mut_arr(cfg, mdoc, key);
        if (!arr) break;
        yyjson_mut_val *strv = yyjson_mut_strncpy(mdoc, value, strlen(value));
        if (!strv) break;
        ok = yyjson_mut_arr_add_val(arr, strv);
    } while (0);
    pthread_mutex_unlock(&cfg->lock);
    return ok;
}

bool config_array_remove(Config *cfg, const char *key, int idx) {
    if (!cfg || idx < 0) return false;
    pthread_mutex_lock(&cfg->lock);
    bool ok = false;
    do {
        yyjson_mut_doc *mdoc = config_mut_doc(cfg);
        if (!mdoc) break;
        yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
        if (!root) break;
        char *k = strdup(key);
        if (!k) break;
        yyjson_mut_val *v = root;
        char *save = NULL;
        char *tok = strtok_r(k, ".", &save);
        while (tok) {
            if (!yyjson_mut_is_obj(v)) { v = NULL; break; }
            v = yyjson_mut_obj_get(v, tok);
            if (!v) break;
            tok = strtok_r(NULL, ".", &save);
        }
        free(k);
        yyjson_mut_val *arr = (v && yyjson_mut_is_arr(v)) ? v : NULL;
        if (!arr) break;
        size_t n = yyjson_mut_arr_size(arr);
        if ((size_t)idx >= n) break;
        ok = yyjson_mut_arr_remove(arr, (size_t)idx) != NULL;
    } while (0);
    pthread_mutex_unlock(&cfg->lock);
    return ok;
}

bool config_save(Config *cfg) {
    if (!cfg || !cfg->path) return false;
    pthread_mutex_lock(&cfg->lock);
    bool ok = false;
    yyjson_write_err werr = {0};
    do {
        yyjson_mut_doc *mdoc = config_mut_doc(cfg);
        if (!mdoc) break;
        yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
        if (!root) break;

        /* Atomic write (tmp + rename): a direct "wb" overwrite can leave a
           half-written config.json if the process is killed mid-write, which
           then fails to parse on the next start and silently resets EVERY user
           setting to defaults. rename() within the same directory is atomic, so
           the config is always either the old complete version or the new
           complete one — a crash leaves at worst a stray .tmp.
           The temp name carries the pid so netune and netune-config cannot
           interleave writes into the same scratch file. */
        char tmp[1400];
#ifdef _WIN32
        int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", cfg->path);
#else
        int tn = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", cfg->path,
                          (long)getpid());
#endif
        if (tn < 0 || tn >= (int)sizeof(tmp)) {
            LOG_WARN("Config path too long: %s", cfg->path);
            break;
        }
        FILE *fp = fopen_utf8(tmp, "wb");
        if (!fp) {
            LOG_WARN("Cannot write config file: %s", cfg->path);
            break;
        }
        ok = yyjson_mut_val_write_fp(fp, root, YYJSON_WRITE_PRETTY,
                                     NULL, &werr);
        if (ok && fflush(fp) != 0) ok = false;
#ifdef _WIN32
        if (ok && _commit(_fileno(fp)) != 0) ok = false;
#else
        if (ok && fsync(fileno(fp)) != 0) ok = false;
#endif
        if (fclose(fp) != 0) ok = false;
        if (ok) {
            if (rename_utf8(tmp, cfg->path) != 0) { remove_utf8(tmp); ok = false; }
        } else {
            remove_utf8(tmp);
        }
    } while (0);
    pthread_mutex_unlock(&cfg->lock);
    if (!ok) LOG_WARN("Failed to serialize config to: %s (code=%d %s)",
                       cfg->path, werr.code, werr.msg ? werr.msg : "");
    return ok;
}

