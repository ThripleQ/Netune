#include "path.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32

#include <windows.h>

/* Windows: manual path expansion (no wordexp available on MSVC).
 * Supports ~ -> %USERPROFILE%, $VAR, and ${VAR} forms.
 * Returns a malloc'd string that the caller must free().
 * On failure, returns strdup(input). */
char* path_expand(const char *path) {
    if (!path) return NULL;

    /* quick path: no shell-reserved chars -> skip expansion */
    if (!strchr(path, '~') && !strchr(path, '$'))
        return strdup(path);

    size_t cap = strlen(path) * 4 + 1;
    char *result = (char*)malloc(cap);
    if (!result) return strdup(path);

    const char *src = path;
    char *dst = result;
    char *buf_end = result + cap - 1;

    while (*src) {
        if (*src == '~' &&
            (src == path || *(src - 1) == '/' || *(src - 1) == '\\')) {
            /* ~ expansion: only at start of path or after a separator */
            char home[MAX_PATH];
            DWORD n = GetEnvironmentVariableA("USERPROFILE", home, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                if (dst + n > buf_end) {
                    size_t used = (size_t)(dst - result);
                    cap = used + n + strlen(src) + 1;
                    char *tmp = (char*)realloc(result, cap);
                    if (!tmp) { free(result); return strdup(path); }
                    result = tmp;
                    dst = result + used;
                    buf_end = result + cap - 1;
                }
                memcpy(dst, home, n);
                dst += n;
                src++;
                continue;
            }
            /* fallback: copy ~ as-is */
            *dst++ = *src++;
        } else if (*src == '$') {
            const char *var_start = src + 1;
            const char *var_end;
            char var_name[256];
            int braced = 0;

            if (*var_start == '{') {
                /* ${VAR} form */
                braced = 1;
                var_start++;
                var_end = var_start;
                while (*var_end && *var_end != '}') var_end++;
                if (*var_end != '}') {
                    /* no closing brace, copy $ as-is */
                    *dst++ = *src++;
                    continue;
                }
            } else {
                /* $VAR form */
                var_end = var_start;
                while (*var_end &&
                       (isalnum((unsigned char)*var_end) || *var_end == '_'))
                    var_end++;
            }

            size_t var_len = (size_t)(var_end - var_start);
            if (var_len == 0 || var_len >= sizeof(var_name)) {
                *dst++ = *src++;
                continue;
            }

            memcpy(var_name, var_start, var_len);
            var_name[var_len] = '\0';

            char value[MAX_PATH];
            DWORD val_len = GetEnvironmentVariableA(var_name, value, MAX_PATH);
            if (val_len > 0 && val_len < MAX_PATH) {
                if (dst + val_len > buf_end) {
                    size_t used = (size_t)(dst - result);
                    cap = used + val_len + strlen(src) + 1;
                    char *tmp = (char*)realloc(result, cap);
                    if (!tmp) { free(result); return strdup(path); }
                    result = tmp;
                    dst = result + used;
                    buf_end = result + cap - 1;
                }
                memcpy(dst, value, val_len);
                dst += val_len;
                src = braced ? var_end + 1 : var_end;
            } else {
                /* variable not found, copy $ as-is */
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return result;
}

#else /* !_WIN32 */

#include <wordexp.h>

char* path_expand(const char *path) {
    if (!path) return NULL;

    /* quick path: no shell-reserved chars -> skip wordexp */
    if (!strchr(path, '~') && !strchr(path, '$'))
        return strdup(path);

    wordexp_t p;
    int ret = wordexp(path, &p, WRDE_NOCMD);
    if (ret != 0 || p.we_wordc == 0) {
        wordfree(&p);
        return strdup(path);
    }

    char *result = strdup(p.we_wordv[0]);
    wordfree(&p);
    return result;
}

#endif /* _WIN32 */
