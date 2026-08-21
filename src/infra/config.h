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

/* global singleton — set by app, read by anyone */
void    config_set_global(Config *cfg);
Config* config_global(void);

/* scalar access (key="section.field") */
const char* config_get_str(Config *cfg, const char *key, const char *fallback);
int         config_get_int(Config *cfg, const char *key, int fallback);
bool        config_get_bool(Config *cfg, const char *key, bool fallback);
double      config_get_double(Config *cfg, const char *key, double fallback);

/* array size */
int         config_get_array_size(Config *cfg, const char *key);

/* key existence check (dotted path resolves to a non-null node) */
bool        config_has(Config *cfg, const char *key);

/* scalar write + persist. config_set_int() updates the in-memory config;
   config_save() writes it back to the file it was loaded from. Returns
   false on failure (e.g. NULL cfg). */
bool        config_set_int(Config *cfg, const char *key, int value);
bool        config_set_str(Config *cfg, const char *key, const char *value);
bool        config_save(Config *cfg);



#ifdef __cplusplus
}
#endif
