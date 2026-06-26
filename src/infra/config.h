#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* ── Opaque config handle ──────────────────────────── */
typedef struct Config Config;

/* ── API ────────────────────────────────────────────── */
Config* config_load(const char *file);       /* NULL on failure */
void    config_free(Config *cfg);

/* scalar access (key="section.field") */
const char* config_get_str(Config *cfg, const char *key, const char *fallback);
int         config_get_int(Config *cfg, const char *key, int fallback);
bool        config_get_bool(Config *cfg, const char *key, bool fallback);
double      config_get_double(Config *cfg, const char *key, double fallback);

/* array size */
int         config_get_array_size(Config *cfg, const char *key);

#ifdef __cplusplus
}
#endif
