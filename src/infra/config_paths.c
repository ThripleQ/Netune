/* config_paths.c — implementation of the shared path helpers.
 * Bodies moved verbatim from app.cpp (xdg_dir / ensure_dir /
 * xdg_data_root) so behaviour is unchanged for the main player while
 * netune-config now gets the same, Windows-aware logic for free. */
#include "infra/config_paths.h"
#include "compat/utf8.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv (MinGW alias branch of utf8.h assumes it) */
#include <string.h>
#include <sys/stat.h> /* mkdir  (same) */

const char *netune_xdg_dir(const char *env, const char *sub) {
    const char *d = getenv_utf8(env);
    static char buf[1024];
#ifndef _WIN32
    if (d && d[0]) {
        snprintf(buf, sizeof(buf), "%s/netune/%s", d, sub ? sub : "");
    } else {
        const char *home = getenv_utf8("HOME");
        if (!home) home = "/tmp";
        const char *prefix = strstr(env, "CONFIG") ? ".config" : ".cache";
        snprintf(buf, sizeof(buf), "%s/%s/netune/%s", home, prefix, sub ? sub : "");
    }
#else
    /* Windows: map XDG_*_HOME to APPDATA / LOCALAPPDATA */
    const char *win_env = NULL;
    if (strstr(env, "CACHE"))
        win_env = "LOCALAPPDATA";
    else if (strstr(env, "CONFIG"))
        win_env = "APPDATA";
    if (win_env) d = getenv_utf8(win_env);
    if (d && d[0]) {
        snprintf(buf, sizeof(buf), "%s\\netune\\%s", d, sub ? sub : "");
    } else {
        const char *home = getenv_utf8("USERPROFILE");
        if (!home) home = "C:\\";
        const char *prefix = strstr(env, "CONFIG") ? ".config" : ".cache";
        snprintf(buf, sizeof(buf), "%s\\%s\\netune\\%s", home, prefix, sub ? sub : "");
    }
#endif
    return buf;
}

void netune_ensure_dir(const char *filepath) {
    /* mkdir -p the parent directory of filepath */
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    /* find last separator (handles both / and \) */
    char *last_slash = strrchr(tmp, '/');
    char *last_bslash = strrchr(tmp, '\\');
    char *last = (last_bslash && last_bslash > last_slash) ? last_bslash : last_slash;
    if (!last) return;
    if (last == tmp) return;  /* root dir, nothing to do */
    char sep_chr = *last;
    *last = 0;  /* strip file name (or trailing separator) */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = 0;
            mkdir_utf8(tmp);
            *p = saved;
        }
    }
    if (tmp[0]) {
        mkdir_utf8(tmp);
    }
    (void)sep_chr;
}

const char *netune_data_root(void) {
    static char buf[1024];
#ifndef _WIN32
    const char *d = getenv_utf8("XDG_CONFIG_HOME");
    if (d && d[0]) {
        snprintf(buf, sizeof(buf), "%s/netune/data", d);
    } else {
        const char *home = getenv_utf8("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, sizeof(buf), "%s/.config/netune/data", home);
    }
#else
    /* Windows: use APPDATA (same mapping as XDG_CONFIG_HOME) */
    const char *d = getenv_utf8("APPDATA");
    if (d && d[0]) {
        snprintf(buf, sizeof(buf), "%s\\netune\\data", d);
    } else {
        const char *home = getenv_utf8("USERPROFILE");
        if (!home) home = "C:\\";
        snprintf(buf, sizeof(buf), "%s\\.config\\netune\\data", home);
    }
#endif
    return buf;
}
