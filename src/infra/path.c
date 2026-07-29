#include "path.h"
#include <string.h>
#include <stdlib.h>
#include <wordexp.h>

/* Expand ~, $VAR and ${VAR} in a path string.
 *
 * POSIX: delegates to libc wordexp() (with WRDE_NOCMD to forbid command
 *        substitution).
 * Windows/MSVC: <wordexp.h> resolves to src/compat/wordexp.h, a minimal
 *        shim that handles ~ (→ USERPROFILE / HOMEDRIVE+HOMEPATH), $VAR and
 *        ${VAR} via the UTF-8 aware getenv_utf8(), so non-ASCII user names
 *        work correctly.
 *
 * Returns a malloc'd string the caller must free(). On failure or when the
 * input contains no expandable token, returns strdup(input). */
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
