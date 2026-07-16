#pragma once
/*
 * Minimal <dirent.h> for MSVC Windows, backed by FindFirstFile/FindNextFile.
 *
 * Provides: DIR, struct dirent, opendir, readdir, closedir.
 * Skips "." and ".." entries automatically.
 */

#ifdef _MSC_VER

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NAME_MAX 260

struct dirent {
    char d_name[NAME_MAX];
};

typedef struct DIR {
    HANDLE          handle;
    WIN32_FIND_DATAA ffd;
    struct dirent   entry;
    int             first;
} DIR;

static inline DIR *opendir(const char *path)
{
    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) return NULL;

    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);

    d->handle = FindFirstFileA(pattern, &d->ffd);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static inline struct dirent *readdir(DIR *d)
{
    if (!d) return NULL;

    for (;;) {
        if (d->first) {
            d->first = 0;
        } else {
            if (!FindNextFileA(d->handle, &d->ffd))
                return NULL;
        }
        /* skip . and .. */
        if (strcmp(d->ffd.cFileName, ".") == 0 ||
            strcmp(d->ffd.cFileName, "..") == 0)
            continue;
        strncpy(d->entry.d_name, d->ffd.cFileName, sizeof(d->entry.d_name) - 1);
        d->entry.d_name[sizeof(d->entry.d_name) - 1] = '\0';
        return &d->entry;
    }
}

static inline int closedir(DIR *d)
{
    if (!d) return -1;
    FindClose(d->handle);
    free(d);
    return 0;
}

#endif /* _MSC_VER */
