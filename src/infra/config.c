#include "config.h"
#include "log.h"
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
};

Config* config_load(const char *path) {
    FILE *fp = fopen_utf8(path, "rb");
    if (!fp) {
        LOG_WARN("Cannot open config file: %s", path);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = (char*)malloc(size + 1);
    if (!buf) { fclose(fp); return NULL; }
    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts(buf, size, YYJSON_READ_ALLOW_COMMENTS, NULL, &err);
    free(buf);

    if (!doc) {
        LOG_ERROR("JSON parse error at pos %zu: %s", (size_t)err.pos, err.msg);
        return NULL;
    }

    Config *cfg = (Config*)calloc(1, sizeof(Config));
    cfg->doc  = doc;
    cfg->root = yyjson_doc_get_root(doc);
    cfg->path = strdup(path);

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
    free(cfg);
}

/* resolve dotted key path: "a.b.c" -> obj["a"]["b"]["c"] */
static yyjson_val* resolve(Config *cfg, const char *key) {
    if (!cfg) return NULL;
    if (!cfg->root) return NULL;
    yyjson_val *v = cfg->root;

    char *k = strdup(key);
    if (!k) return NULL;
    char *tok = strtok(k, ".");
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

        tok = strtok(NULL, ".");
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
    char *tok = strtok(k, ".");
    while (tok && v) {
        v = yyjson_mut_is_obj(v) ? yyjson_mut_obj_get(v, tok) : NULL;
        tok = strtok(NULL, ".");
    }
    free(k);
    return v;
}

const char* config_get_str(Config *cfg, const char *key, const char *fallback) {
    if (!cfg) return fallback;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_str(mv)) return yyjson_mut_get_str(mv);
    yyjson_val *v = resolve(cfg, key);
    if (!v || !yyjson_is_str(v)) return fallback;
    return yyjson_get_str(v);
}

int config_get_int(Config *cfg, const char *key, int fallback) {
    if (!cfg) return fallback;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_int(mv)) return (int)yyjson_mut_get_int(mv);
    yyjson_val *v = resolve(cfg, key);
    if (!v || !yyjson_is_int(v)) return fallback;
    return (int)yyjson_get_int(v);
}

bool config_get_bool(Config *cfg, const char *key, bool fallback) {
    if (!cfg) return fallback;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_bool(mv)) return yyjson_mut_get_bool(mv);
    yyjson_val *v = resolve(cfg, key);
    if (!v || !yyjson_is_bool(v)) return fallback;
    return yyjson_get_bool(v);
}

double config_get_double(Config *cfg, const char *key, double fallback) {
    if (!cfg) return fallback;
    yyjson_mut_val *mv = resolve_mut_read(cfg, key);
    if (mv && yyjson_mut_is_num(mv)) return yyjson_mut_get_num(mv);
    yyjson_val *v = resolve(cfg, key);
    if (!v || !(yyjson_is_num(v))) return fallback;
    return yyjson_get_num(v);
}

int config_get_array_size(Config *cfg, const char *key) {
    if (!cfg) return 0;
    yyjson_val *v = resolve(cfg, key);
    if (!v || !yyjson_is_arr(v)) return 0;
    return (int)yyjson_arr_size(v);
}

bool config_has(Config *cfg, const char *key) {
    if (!cfg || !key) return false;
    return resolve(cfg, key) != NULL;
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
    char *tok = strtok(k, ".");
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
        tok = strtok(NULL, ".");
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
    yyjson_mut_doc *mdoc = config_mut_doc(cfg);
    if (!mdoc) return false;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
    if (!root) return false;

    const char *leaf_key = NULL;
    yyjson_mut_val *leaf = NULL;
    yyjson_mut_val *parent = resolve_mut_path(cfg, mdoc, root, key,
                                              &leaf_key, &leaf);
    if (!parent || !leaf_key) return false;

    yyjson_mut_set_int(leaf, value);
    return true;
}

bool config_set_str(Config *cfg, const char *key, const char *value) {
    if (!cfg) return false;
    yyjson_mut_doc *mdoc = config_mut_doc(cfg);
    if (!mdoc) return false;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
    if (!root) return false;

    const char *leaf_key = NULL;
    yyjson_mut_val *leaf = NULL;
    yyjson_mut_val *parent = resolve_mut_path(cfg, mdoc, root, key,
                                              &leaf_key, &leaf);
    if (!parent || !leaf_key) return false;

    /* yyjson_mut_set_str only stores the pointer without copying.  Callers
       may pass transient buffers (std::string::c_str() etc), so copy the
       string into the doc arena instead of keeping a dangling reference.
       The leaf node keeps its own tag/next chain — only payload (tag+uni)
       is replaced, since the string data must outlive the caller's buffer. */
    size_t len = value ? strlen(value) : 0;
    yyjson_mut_val *nv = yyjson_mut_strncpy(mdoc, value, len);
    if (!nv) return false;
    leaf->tag = nv->tag;
    leaf->uni = nv->uni;
    return true;
}

bool config_save(Config *cfg) {
    if (!cfg || !cfg->path) return false;
    yyjson_mut_doc *mdoc = config_mut_doc(cfg);
    if (!mdoc) return false;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
    if (!root) return false;

    /* write via fopen_utf8 so UTF-8 paths work on Windows too */
    FILE *fp = fopen_utf8(cfg->path, "wb");
    if (!fp) {
        LOG_WARN("Cannot write config file: %s", cfg->path);
        return false;
    }
    yyjson_write_err werr = {0};
    bool ok = yyjson_mut_val_write_fp(fp, root, YYJSON_WRITE_PRETTY,
                                      NULL, &werr);
    fclose(fp);
    if (!ok) LOG_WARN("Failed to serialize config to: %s (code=%d %s)",
                       cfg->path, werr.code, werr.msg ? werr.msg : "");
    return ok;
}

