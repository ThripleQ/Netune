#include "path.h"
#include <string.h>
#include <stdlib.h>
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
