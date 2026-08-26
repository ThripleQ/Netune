#pragma once
/*
 * UTF-8 aware file I/O wrappers for MSVC Windows.
 *
 * Problem: On Windows, getenv() returns strings in the ANSI code page,
 * and fopen()/mkdir()/access()/stat() also use the ANSI code page.
 * However, the compat dirent.h shim converts paths using MultiByteToWideChar
 * with CP_UTF8, expecting UTF-8 input. This mismatch causes failures when
 * paths contain non-ASCII characters (e.g. Chinese usernames in APPDATA).
 *
 * Solution: These wrappers use _wgetenv() to get environment variables as
 * UTF-16, convert to UTF-8 for internal use, and use the wide-character
 * (_w*) Win32/CRT APIs for file operations. This ensures that all internal
 * paths are UTF-8, and all system calls handle Unicode correctly.
 *
 * On POSIX/MinGW, these are transparent aliases to the standard C library.
 * MinGW-w64's CRT uses UTF-8 directly, so no conversion is needed.
 *
 * Usage: Include "utf8.h" and replace:
 *   getenv("X")     -> getenv_utf8("X")
 *   fopen(p, m)     -> fopen_utf8(p, m)
 *   mkdir(p, 0755)   -> mkdir_utf8(p)
 *   _mkdir(p)        -> mkdir_utf8(p)
 *   access(p, F_OK) -> access_utf8(p, F_OK)
 *   _access(p, m)   -> access_utf8(p, m)
 *   stat(p, &st)    -> stat_utf8(p, &st)
 *   remove(p)        -> remove_utf8(p)
 *   rename(a, b)     -> rename_utf8(a, b)
 *
 * F_OK, R_OK, W_OK are defined if not already available.
 */

#ifdef _MSC_VER

/* Same windows.h hygiene as compat/dirent.h: NOGDI avoids wingdi's RGB
 * macro clobbering FTXUI's Color::RGB in C++ TUs that include this header. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <wchar.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

/* ── access() mode flags — not defined by MSVC ──────── */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 6
#endif

/* ── S_ISDIR/S_ISREG — not provided by MSVC's <sys/stat.h> ── */
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & _S_IFMT) == _S_IFREG)
#endif

/* ── Internal conversion helpers ────────────────────── */

/* Convert a UTF-8 string to a UTF-16 wide string.
   Returns the number of wide chars written (including NUL), or 0 on failure. */
static inline int utf8_to_wide(const char *utf8, wchar_t *wbuf, int wbuf_size)
{
    if (!utf8 || !wbuf || wbuf_size <= 0) return 0;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wbuf_size);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_ACP, 0, utf8, -1, wbuf, wbuf_size);
        if (len <= 0) return 0;
    }
    wbuf[wbuf_size - 1] = L'\0';
    return len;
}

/* ── getenv_utf8 ────────────────────────────────────── */
static inline const char* getenv_utf8(const char *name)
{
    if (!name) return NULL;
    wchar_t wname[256];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256) == 0)
        return NULL;
    const wchar_t *wval = _wgetenv(wname);
    if (!wval || !wval[0]) return NULL;
    static __declspec(thread) char buf[32768];
    if (WideCharToMultiByte(CP_UTF8, 0, wval, -1, buf, (int)sizeof(buf),
                             NULL, NULL) == 0)
        return NULL;
    return buf;
}

/* ── fopen_utf8 ─────────────────────────────────────── */
static inline FILE* fopen_utf8(const char *path, const char *mode)
{
    if (!path || !mode) return NULL;
    wchar_t wpath[32768];
    wchar_t wmode[16];
    if (utf8_to_wide(path, wpath, 32768) == 0) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16) == 0)
        return NULL;
    return _wfopen(wpath, wmode);
}

/* ── mkdir_utf8 ─────────────────────────────────────── */
static inline int mkdir_utf8(const char *path)
{
    if (!path) return -1;
    wchar_t wpath[32768];
    if (utf8_to_wide(path, wpath, 32768) == 0) return -1;
    return _wmkdir(wpath);
}

/* ── access_utf8 ────────────────────────────────────── */
static inline int access_utf8(const char *path, int mode)
{
    if (!path) return -1;
    wchar_t wpath[32768];
    if (utf8_to_wide(path, wpath, 32768) == 0) return -1;
    return _waccess(wpath, mode);
}

/* ── stat_utf8 ──────────────────────────────────────── */
static inline int stat_utf8(const char *path, struct stat *buf)
{
    if (!path || !buf) return -1;
    wchar_t wpath[32768];
    if (utf8_to_wide(path, wpath, 32768) == 0) return -1;
    return _wstat(wpath, (struct _stat*)buf);
}

/* ── remove_utf8 ────────────────────────────────────── */
static inline int remove_utf8(const char *path)
{
    if (!path) return -1;
    wchar_t wpath[32768];
    if (utf8_to_wide(path, wpath, 32768) == 0) return -1;
    return _wremove(wpath);
}

/* ── rename_utf8 ────────────────────────────────────── */
static inline int rename_utf8(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath) return -1;
    wchar_t wold[32768], wnew[32768];
    if (utf8_to_wide(oldpath, wold, 32768) == 0) return -1;
    if (utf8_to_wide(newpath, wnew, 32768) == 0) return -1;
    return _wrename(wold, wnew);
}

/* ── truncate_utf8 ──────────────────────────────────── */
static inline int truncate_utf8(const char *path, long long size)
{
    if (!path || size < 0) return -1;
    wchar_t wpath[32768];
    if (utf8_to_wide(path, wpath, 32768) == 0) return -1;
    int fd = _wopen(wpath, _O_WRONLY | _O_BINARY);
    if (fd < 0) return -1;
    int rc = _chsize_s(fd, size);
    _close(fd);
    return rc;
}

#elif defined(__MINGW32__)

/* MinGW-w64: CRT uses UTF-8 directly, no conversion needed.
   Provide transparent aliases for API compatibility. */
#include <direct.h>   /* _mkdir */
#include <fcntl.h>    /* _O_WRONLY / _O_BINARY */
#include <io.h>       /* _open / _chsize */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 6
#endif

#define getenv_utf8  getenv
#define fopen_utf8   fopen
/* MinGW-w64's mkdir() is the deprecated single-arg MSVC shim;
   _mkdir() is the correct single-arg CRT entry point. */
#define mkdir_utf8(p) _mkdir(p)
#define access_utf8  access
#define stat_utf8    stat
#define remove_utf8  remove
#define rename_utf8  rename

static inline int truncate_utf8(const char *path, long long size)
{
    if (!path || size < 0) return -1;
    int fd = _open(path, _O_WRONLY | _O_BINARY);
    if (fd < 0) return -1;
    int rc = _chsize(fd, (long)size);
    _close(fd);
    return rc;
}

#else /* POSIX */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 6
#endif

#define getenv_utf8  getenv
#define fopen_utf8   fopen
#define mkdir_utf8(p) mkdir((p), 0755)
#define access_utf8  access
#define stat_utf8    stat
#define remove_utf8  remove
#define rename_utf8  rename

static inline int truncate_utf8(const char *path, long long size)
{
    if (!path || size < 0) return -1;
    return truncate(path, (off_t)size);
}

#endif
