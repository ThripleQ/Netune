#include "path.h"
#include "compat/utf8.h"   /* getenv_utf8 on Windows */
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/* Growable output buffer for the expansion. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    oom;
} Out;

static void out_put(Out *o, const char *s, size_t n) {
    if (o->oom) return;
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 256;
        while (cap < o->len + n + 1) cap *= 2;
        char *p = (char*)realloc(o->buf, cap);
        if (!p) { o->oom = 1; return; }
        o->buf = p;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void out_cstr(Out *o, const char *s) { out_put(o, s, strlen(s)); }

static void put_env(Out *o, const char *name, size_t n) {
    if (n == 0 || n >= 256) {          /* not a plausible variable name */
        out_put(o, "$", 1);
        out_put(o, name, n);
        return;
    }
    char key[256];
    memcpy(key, name, n);
    key[n] = '\0';
    const char *v = getenv_utf8(key);
    if (v) out_cstr(o, v);
    else {                             /* unknown variable: keep literal */
        out_put(o, "$", 1);
        out_put(o, name, n);
    }
}

/* Expand ~, $VAR and ${VAR} in a path string.
 *
 * Deliberately NOT wordexp(): that applies shell field splitting and glob
 * expansion, so a perfectly valid directory like "~/Music [FLAC]" came back
 * truncated/mangled. Paths are used verbatim; only the leading ~ and
 * environment references are substituted.
 *
 * Returns a malloc'd string the caller must free(). On failure returns
 * strdup(input) (NULL only if allocation itself fails). */
char* path_expand(const char *path) {
    if (!path) return NULL;

    /* quick path: no expandable token */
    if (!strchr(path, '~') && !strchr(path, '$'))
        return strdup(path);

    Out o = {0};
    const char *p = path;

    if (*p == '~' && (p[1] == '\0' || p[1] == '/' || p[1] == '\\')) {
        const char *home = getenv_utf8("HOME");
#ifdef _WIN32
        if (!home || !home[0]) home = getenv_utf8("USERPROFILE");
#endif
        if (home && home[0]) {
            out_cstr(&o, home);
            p++;
        }
    }

    while (*p) {
        if (*p == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            const char *name = p + 1;
            const char *e = name;
            while (isalnum((unsigned char)*e) || *e == '_') e++;
            put_env(&o, name, (size_t)(e - name));
            p = e;
        } else if (*p == '$' && p[1] == '{') {
            const char *name = p + 2;
            const char *e = strchr(name, '}');
            if (e) {
                put_env(&o, name, (size_t)(e - name));
                p = e + 1;
            } else {
                out_put(&o, "$", 1);
                p++;
            }
        } else {
            out_put(&o, p, 1);
            p++;
        }
    }

    if (o.oom) {
        free(o.buf);
        return strdup(path);
    }
    if (!o.buf) return strdup(path);
    return o.buf;
}
