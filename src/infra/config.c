#include "config.h"
#include "log.h"
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Config {
    yyjson_doc  *doc;
    yyjson_val  *root;
};

Config* config_load(const char *path) {
    FILE *fp = fopen(path, "rb");
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
    yyjson_doc *doc = yyjson_read_opts(buf, size, 0, NULL, &err);
    free(buf);

    if (!doc) {
        LOG_ERROR("JSON parse error at pos %zu: %s", (size_t)err.pos, err.msg);
        return NULL;
    }

    Config *cfg = (Config*)calloc(1, sizeof(Config));
    cfg->doc  = doc;
    cfg->root = yyjson_doc_get_root(doc);

    LOG_INFO("Config loaded: %s", path);
    return cfg;
}

void config_free(Config *cfg) {
    if (!cfg) return;
    yyjson_doc_free(cfg->doc);
    free(cfg);
}

/* resolve dotted key path: "a.b.c" -> obj["a"]["b"]["c"] */
static yyjson_val* resolve(Config *cfg, const char *key) {
    if (!cfg->root) return NULL;
    yyjson_val *v = cfg->root;

    char *k = strdup(key);
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

const char* config_get_str(Config *cfg, const char *key, const char *fallback) {
    yyjson_val *v = resolve(cfg, key);
    if (!v || !yyjson_is_str(v)) return fallback;
    return yyjson_get_str(v);
}

int config_get_int(Config *cfg, const char *key, int fallback) {
    yyjson_val *v = resolve(cfg, key);
    if (!v || !yyjson_is_int(v)) return fallback;
    return (int)yyjson_get_int(v);
}

bool config_get_bool(Config *cfg, const char *key, bool fallback) {
    yyjson_val *v = resolve(cfg, key);
    if (!v || !yyjson_is_bool(v)) return fallback;
    return yyjson_get_bool(v);
}

double config_get_double(Config *cfg, const char *key, double fallback) {
    yyjson_val *v = resolve(cfg, key);
    if (!v || !(yyjson_is_num(v))) return fallback;
    return yyjson_get_num(v);
}

int config_get_array_size(Config *cfg, const char *key) {
    yyjson_val *v = resolve(cfg, key);
    if (!v || !yyjson_is_arr(v)) return 0;
    return (int)yyjson_arr_size(v);
}
