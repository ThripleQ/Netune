#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Expand ~, $HOME, ${VAR} etc. in a path string using wordexp.
   Returns a malloc'd string that the caller must free().
   On failure, returns strdup(input). */
char* path_expand(const char *path);

#ifdef __cplusplus
}
#endif
