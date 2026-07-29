#pragma once
/*
 * Minimal <dirent.h> for MSVC Windows, backed by FindFirstFileW/FindNextFileW.
 *
 * Provides: DIR, struct dirent, opendir, readdir, closedir.
 * Skips "." and ".." entries automatically.
 *
 * Uses the wide-character (W) Win32 APIs so that Unicode / non-ASCII paths
 * (e.g. Chinese directory names) work correctly. The caller passes and
 * receives UTF-8 strings; conversion to/from UTF-16 is handled internally.
 */

#ifdef _WIN32

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define NAME_MAX 260

struct dirent {
    char d_name[NAME_MAX];
};

typedef struct DIR {
    HANDLE           handle;
    WIN32_FIND_DATAW ffd;
    struct dirent    entry;
    int              first;
} DIR;

/* Convert a UTF-8 string to a newly allocated UTF-16 wide string.
   Returns NULL on failure. Caller must free() the result. */
static inline wchar_t *dirent_utf8_to_wide(const char *utf8)
{
    if (!utf8) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wlen);
    return w;
}

/* Convert a UTF-16 wide string to UTF-8 in the provided buffer.
   Returns 0 on success, -1 on failure or truncation. */
static inline int dirent_wide_to_utf8(const wchar_t *w, char *buf, size_t buf_size)
{
    if (!w || !buf || buf_size == 0) return -1;
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)buf_size, NULL, NULL);
    if (len <= 0) return -1;
    /* WideCharToMultiByte writes the NUL terminator; ensure truncation safety */
    buf[buf_size - 1] = '\0';
    return 0;
}

static inline DIR *opendir(const char *path)
{
    if (!path) return NULL;

    /* Build "<path>\*" in UTF-16 */
    wchar_t *wpath = dirent_utf8_to_wide(path);
    if (!wpath) return NULL;

    size_t wlen = wcslen(wpath);
    wchar_t *pattern = (wchar_t *)malloc((wlen + 3) * sizeof(wchar_t));
    if (!pattern) { free(wpath); return NULL; }
    wcscpy(pattern, wpath);
    /* Ensure exactly one backslash before '*' */
    if (wlen > 0 && pattern[wlen - 1] != L'\\' && pattern[wlen - 1] != L'/')
        pattern[wlen] = L'\\', pattern[wlen + 1] = L'*', pattern[wlen + 2] = L'\0';
    else
        pattern[wlen] = L'*', pattern[wlen + 1] = L'\0';
    free(wpath);

    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) { free(pattern); return NULL; }

    d->handle = FindFirstFileW(pattern, &d->ffd);
    free(pattern);
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
            if (!FindNextFileW(d->handle, &d->ffd))
                return NULL;
        }
        /* skip . and .. */
        if (wcscmp(d->ffd.cFileName, L".") == 0 ||
            wcscmp(d->ffd.cFileName, L"..") == 0)
            continue;
        /* Convert the wide filename to UTF-8 */
        if (dirent_wide_to_utf8(d->ffd.cFileName, d->entry.d_name,
                                sizeof(d->entry.d_name)) != 0) {
            /* conversion failed — return an empty name rather than crashing */
            d->entry.d_name[0] = '\0';
        }
        return &d->entry;
    }
}

static inline int closedir(DIR *d)
{
    if (!d) return -1;
    if (d->handle != INVALID_HANDLE_VALUE)
        FindClose(d->handle);
    free(d);
    return 0;
}

#endif /* _WIN32 */
