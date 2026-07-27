#pragma once
/*
 * Minimal <wordexp.h> for MSVC Windows.
 *
 * POSIX wordexp() performs shell-style word expansion. This shim implements
 * just enough for netune's path_expand():
 *   - ~           → USERPROFILE (or HOMEDRIVE+HOMEPATH)
 *   - ~user       → USERPROFILE (other users not supported)
 *   - $VAR        → getenv("VAR")
 *   - ${VAR}      → getenv("VAR")
 *
 * Shell metacharacters (|, &, ;, <, >, (, ), `, quotes) are NOT expanded —
 * if present, the original string is returned unchanged.
 *
 * Only compiled on MSVC; POSIX platforms use the real <wordexp.h>.
 */

#ifdef _MSC_VER

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include "utf8.h"

#define WRDE_NOCMD 0

typedef struct {
    size_t  we_wordc;   /* count of words */
    char  **we_wordv;   /* array of words */
    size_t  we_offs;    /* reserved slots at start of we_wordv */
} wordexp_t;

static inline int wordexp(const char *words, wordexp_t *p, int flags)
{
    (void)flags;
    if (!words || !p) return -1;

    /* If the string contains shell metacharacters we don't handle, bail out */
    if (strpbrk(words, "|&;<>()`'\"\\"))
        return -1;

    /* Allocate a single result word */
    char *result = (char *)malloc(strlen(words) + 1);
    if (!result) return -1;

    const char *src = words;
    char *dst = result;
    size_t cap = strlen(words) + 1;
    size_t used = 0;

    while (*src) {
        /* Need room for at least 1 char + NUL */
        if (used + 2 > cap) {
            cap *= 2;
            char *t = (char *)realloc(result, cap);
            if (!t) { free(result); return -1; }
            result = t;
            dst = result + used;
        }

        if (*src == '~') {
            /* Expand ~ to the user's home directory */
            const char *home = getenv_utf8("USERPROFILE");
            if (!home || !home[0]) {
                /* Fallback: HOMEDRIVE + HOMEPATH */
                const char *hd = getenv_utf8("HOMEDRIVE");
                const char *hp = getenv_utf8("HOMEPATH");
                if (hd && hp) {
                    size_t hl = strlen(hd) + strlen(hp) + 1;
                    while (used + hl + 1 > cap) {
                        cap *= 2;
                        char *t = (char *)realloc(result, cap);
                        if (!t) { free(result); return -1; }
                        result = t;
                        dst = result + used;
                    }
                    size_t dl = strlen(hd);
                    memcpy(dst, hd, dl);
                    memcpy(dst + dl, hp, strlen(hp));
                    used += dl + strlen(hp);
                    dst = result + used;
                    src++;
                    continue;
                }
                home = "\\";
            }
            size_t hl = strlen(home);
            while (used + hl + 1 > cap) {
                cap *= 2;
                char *t = (char *)realloc(result, cap);
                if (!t) { free(result); return -1; }
                result = t;
                dst = result + used;
            }
            memcpy(dst, home, hl);
            used += hl;
            dst = result + used;
            src++;
        } else if (*src == '$') {
            /* Expand $VAR or ${VAR} */
            src++;
            char varname[256];
            size_t vi = 0;
            int braced = 0;
            if (*src == '{') { braced = 1; src++; }
            while (*src && (isalnum((unsigned char)*src) || *src == '_') &&
                   vi < sizeof(varname) - 1) {
                varname[vi++] = *src++;
            }
            varname[vi] = '\0';
            if (braced && *src == '}') src++;

            const char *val = varname[0] ? getenv_utf8(varname) : NULL;
            if (val) {
                size_t vl = strlen(val);
                while (used + vl + 1 > cap) {
                    cap *= 2;
                    char *t = (char *)realloc(result, cap);
                    if (!t) { free(result); return -1; }
                    result = t;
                    dst = result + used;
                }
                memcpy(dst, val, vl);
                used += vl;
                dst = result + used;
            }
            /* If var not found, expand to empty string */
        } else {
            *dst++ = *src++;
            used++;
        }
    }

    *dst = '\0';

    p->we_wordv = (char **)malloc(sizeof(char *) * 2);
    if (!p->we_wordv) { free(result); return -1; }
    p->we_wordv[0] = result;
    p->we_wordv[1] = NULL;
    p->we_wordc = 1;
    p->we_offs = 0;
    return 0;
}

static inline void wordfree(wordexp_t *p)
{
    if (!p) return;
    if (p->we_wordv) {
        for (size_t i = 0; i < p->we_wordc; i++)
            free(p->we_wordv[i]);
        free(p->we_wordv);
        p->we_wordv = NULL;
    }
    p->we_wordc = 0;
}

#endif /* _MSC_VER */
