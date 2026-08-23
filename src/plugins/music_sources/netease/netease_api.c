#include "netease_api.h"
#include "netease_quality.h"
#include "infra/log.h"
#include "infra/config_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compat/utf8.h"
#include "compat/strings.h"   /* strncasecmp on MSVC -> _strnicmp */
#ifndef _WIN32
#include <unistd.h>
#define STDERR_REDIRECT " 2>/dev/null"
#else
#include <windows.h>
#include <wchar.h>
#include <io.h>
#include <process.h>
#define STDERR_REDIRECT " 2>NUL"
#endif
#include <stdarg.h>
#include <yyjson.h>

/* ── Cross-platform path separator ─────────────────── */
#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

/* ── netease-cli binary resolution ───────────────────
   Priority: (1) netease-cli sitting next to the netune executable
   (that is where CMake builds it — build/netease-cli), (2) PATH.
   Resolved once and cached; quoted when absolute so paths with
   spaces survive the popen shell. */
#define CLI cli_path()

static char g_cli[1024] = "";
static char g_name[128] = "";

static const char *cli_path(void) {
    if (g_cli[0]) return g_cli;
#ifndef _WIN32
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            /* bare path for the existence check, quoted for popen.
               Skip if the joined path would overflow the buffer —
               PATH lookup is the fallback. */
            char bare[1024];
            int written = snprintf(bare, sizeof(bare), "%s/netease-cli", exe);
            if (written > 0 && written < (int)sizeof(bare) - 3 &&
                access(bare, X_OK) == 0) {
                int n2 = snprintf(g_cli, sizeof(g_cli), "\"%s\"", bare);
                if (n2 > 0 && n2 < (int)sizeof(g_cli))
                    return g_cli;
                /* overflow: fall through to PATH lookup */
            }
        }
    }
#else
    wchar_t wbuf[MAX_PATH];
    if (GetModuleFileNameW(NULL, wbuf, MAX_PATH) > 0) {
        for (int i = (int)wcslen(wbuf) - 1; i >= 0; i--) {
            if (wbuf[i] == L'\\' || wbuf[i] == L'/') { wbuf[i + 1] = L'\0'; break; }
        }
        wchar_t wbare[MAX_PATH + 32];
        _snwprintf(wbare, MAX_PATH + 32, L"%snetease-cli.exe", wbuf);
        wbare[MAX_PATH + 32 - 1] = L'\0';
        /* wide existence check — survives non-ASCII (e.g. Chinese
           username) install paths that GetFileAttributesA would miss */
        if (GetFileAttributesW(wbare) != INVALID_FILE_ATTRIBUTES) {
            char bare[1024];
            WideCharToMultiByte(CP_UTF8, 0, wbare, -1, bare, sizeof(bare), NULL, NULL);
            snprintf(g_cli, sizeof(g_cli), "\"%s\"", bare);
            return g_cli;
        }
    }
#endif
    /* fallback: rely on PATH */
    snprintf(g_cli, sizeof(g_cli), "netease-cli");
    return g_cli;
}

/* ── shell escaping (prevents command injection) ────
   Wraps user/API-provided strings so they are safe to embed in the
   netease-cli command line.  Returns a malloc'd string the caller
   must free().

   POSIX  : single-quote wrapping for /bin/sh (used by popen()).
   Windows: double-quote wrapping following the CreateProcess /
            CommandLineToArgvW rules — backslashes are doubled only
            when they precede a double quote (or end the argument),
            embedded double quotes become \".  The result is passed to
            CreateProcessW directly (see run()), NOT to cmd.exe. */
static char *shell_escape(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    /* POSIX: wrap in single quotes, escape embedded quotes as '\'' */
#ifndef _WIN32
    size_t cap = len * 4 + 3;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            out[j++] = '\''; out[j++] = '\\'; out[j++] = '\''; out[j++] = '\'';
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
#else
    /* Worst case every byte doubles (backslash run before a quote, or
       the quote itself) plus the wrapping quotes and NUL. */
    size_t cap = len * 2 + 3;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '"';
    size_t i = 0;
    while (i < len) {
        if (s[i] == '\\') {
            size_t nb = 0;
            while (i < len && s[i] == '\\') { nb++; i++; }
            /* Backslashes are literal unless followed by a quote (or the
               end of the argument, right before our closing quote). */
            int special = (i == len || s[i] == '"');
            for (size_t k = 0; k < (special ? nb * 2 : nb); k++) out[j++] = '\\';
            if (i < len && s[i] == '"') { out[j++] = '\\'; out[j++] = '"'; i++; }
        } else if (s[i] == '"') {
            out[j++] = '\\'; out[j++] = '"'; i++;
        } else {
            out[j++] = s[i]; i++;
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
#endif
}

#ifndef _WIN32
#define shell_escape_cmd shell_escape
#else
/* cmd.exe escaping — only for system() based commands (netease_download).
   Do NOT use for the netease-cli invocation: that goes through
   CreateProcessW, which needs shell_escape() above. */
static char *shell_escape_cmd(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    size_t cap = len * 2 + 3;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') {
            out[j++] = '\\'; out[j++] = '"';
        } else if (s[i] == '^' || s[i] == '&' || s[i] == '|' ||
                   s[i] == '<' || s[i] == '>' || s[i] == '%') {
            out[j++] = '^'; out[j++] = s[i];
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}
#endif

/* ── CLI runner ─────────────────────────────────────
   POSIX  : popen() through /bin/sh.
   Windows: CreateProcessW with an anonymous stdout pipe.  Deliberately
            does NOT go through cmd.exe (as _popen/system do): cmd's
            quote-stripping rules mangle command lines of the form
                "C:\...\netease-cli.exe" <subcmd> "<arg>"
            (more than two quote characters → cmd strips the first and
            last quote of the whole line), which made every netease-cli
            invocation fail.  CreateProcessW parses the quoted executable
            path itself and needs no shell.  stdin is pointed at NUL so
            the child can never block waiting for input; stderr goes to
            NUL as well (the old " 2>NUL" suffix is stripped below). */
#ifdef _WIN32
static char *run_createprocess(const char *cmd) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *wcmd = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wcmd) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, wlen);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) { free(wcmd); return NULL; }
    /* read end must not be inherited by the child */
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNulWr = CreateFileW(L"NUL", GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        OPEN_EXISTING, 0, NULL);
    HANDLE hNulRd = CreateFileW(L"NUL", GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hNulRd ? hNulRd : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWrite;
    si.hStdError  = hNulWr ? hNulWr : hWrite;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    CloseHandle(hWrite);
    if (hNulWr) CloseHandle(hNulWr);
    if (hNulRd) CloseHandle(hNulRd);

    if (!ok) {
        LOG_WARN("netease-cli CreateProcessW failed (err=%lu)", GetLastError());
        CloseHandle(hRead); free(wcmd); return NULL;
    }

    size_t cap = 8192, len = 0;
    char *b = (char*)malloc(cap);
    if (!b) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        CloseHandle(hRead); free(wcmd);
        return NULL;
    }
    char tmp[4096];
    DWORD r = 0;
    while (ReadFile(hRead, tmp, sizeof(tmp), &r, NULL) && r > 0) {
        if (len + r + 1 >= cap) {
            size_t ncap = cap * 2;
            if (ncap < len + r + 1) ncap = len + r + 1;
            char *t = (char*)realloc(b, ncap);
            if (!t) { free(b); b = NULL; break; }
            b = t; cap = ncap;
        }
        memcpy(b + len, tmp, r);
        len += r;
    }
    if (b) b[len] = '\0';
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    free(wcmd);
    return b;
}
#endif

static char *run(const char *fmt, ...) {
    char cmd[4096]; va_list ap;
    va_start(ap, fmt); vsnprintf(cmd, sizeof(cmd), fmt, ap); va_end(ap);
#ifdef _WIN32
    /* drop the cmd.exe-style stderr redirect; run_createprocess()
       already routes the child's stderr to NUL */
    size_t clen = strlen(cmd);
    const char redir[] = " 2>NUL";
    const size_t rlen = sizeof(redir) - 1;
    if (clen >= rlen && strcmp(cmd + clen - rlen, redir) == 0)
        cmd[clen - rlen] = '\0';
    return run_createprocess(cmd);
#else
    /* Wrap in `timeout` so a hung netease-cli (network requests have no
       internal deadline) can never freeze the UI thread. */
    char tcmd[4096 + 64];
    snprintf(tcmd, sizeof(tcmd), "timeout 8 %s", cmd);
    FILE *fp = popen(tcmd, "r"); if (!fp) return NULL;
    size_t cap = 8192, len = 0; char *b = malloc(cap); if (!b) { pclose(fp); return NULL; }
    while (!feof(fp)) {
        if (len+1024>=cap) { cap*=2; char*t=realloc(b,cap); if(!t){free(b);pclose(fp);return NULL;} b=t; }
        size_t r=fread(b+len,1,cap-len-1,fp); if(r>0)len+=r; else break;
    }
    b[len]=0; pclose(fp); return b;
#endif
}

/* ── JSON extraction helpers (yyjson-based) ───────── */
/* Get a string value from an object; returns NULL if missing or wrong type.
   Caller does NOT free the result (yyjson owns it). */
static const char *jget_str(yyjson_val *obj, const char *key) {
    if (!obj) return NULL;
    yyjson_val *v = yyjson_obj_get(obj, key);
    return (v && yyjson_is_str(v)) ? yyjson_get_str(v) : NULL;
}
static long long jget_int(yyjson_val *obj, const char *key) {
    if (!obj) return 0;
    yyjson_val *v = yyjson_obj_get(obj, key);
    return v ? yyjson_get_int(v) : 0;
}
static bool jget_bool(yyjson_val *obj, const char *key) {
    if (!obj) return false;
    yyjson_val *v = yyjson_obj_get(obj, key);
    return v ? yyjson_get_bool(v) : false;
}
static int64_t jget_sint64(yyjson_val *obj, const char *key) {
    if (!obj) return 0;
    yyjson_val *v = yyjson_obj_get(obj, key);
    return v ? yyjson_get_sint(v) : 0;
}
/* Get a sub-object from an object; returns NULL if missing or not an object. */
static yyjson_val *jget_obj(yyjson_val *obj, const char *key) {
    if (!obj) return NULL;
    yyjson_val *v = yyjson_obj_get(obj, key);
    return (v && yyjson_is_obj(v)) ? v : NULL;
}
/* Get a sub-array from an object; returns NULL if missing or not an array. */
static yyjson_val *jget_arr(yyjson_val *obj, const char *key) {
    if (!obj) return NULL;
    yyjson_val *v = yyjson_obj_get(obj, key);
    return (v && yyjson_is_arr(v)) ? v : NULL;
}
/* jfirst_obj removed — use yyjson_arr_get_first() directly */

/* ── parse one song from a yyjson_val object ──────── */
static void fill(SongInfo *s, yyjson_val *song) {
    memset(s,0,sizeof(*s));
    s->source            = strdup("netease");
    s->aux_label         = strdup("");

    int64_t sid = jget_sint64(song, "id");
    char idbuf[32]; snprintf(idbuf, sizeof(idbuf), "%ld", (long)sid);
    s->id = strdup(idbuf);
    const char *name = jget_str(song, "name"); s->title = name ? strdup(name) : strdup("");

    /* artist from ar[0].name */
    yyjson_val *ar = jget_arr(song, "ar") ? jget_arr(song, "ar") : jget_arr(song, "artists");
    if (ar) {
        yyjson_val *first = yyjson_arr_get_first(ar);
        if (first && yyjson_is_obj(first)) {
            const char *an = jget_str(first, "name");
            s->artist = an ? strdup(an) : strdup("");
        } else s->artist = strdup("");
    } else s->artist = strdup("");

    /* album from al.name */
    yyjson_val *al = jget_obj(song, "al") ? jget_obj(song, "al") : jget_obj(song, "album");
    if (al) {
        const char *an = jget_str(al, "name");
        s->album = an ? strdup(an) : strdup("");
        const char *pu = jget_str(al, "picUrl");
        if (pu) { free(s->cover_url); s->cover_url = strdup(pu); }
    } else s->album = strdup("");

    s->duration_sec = (int)(jget_int(song, "dt") / 1000);
    s->fee = (int)jget_int(song, "fee");
    if (!s->cover_url || !s->cover_url[0]) s->cover_url = strdup("");
}

/* ── parse songs array ─────────────────────────────── */
static int parselist(const char *json, const char *loc, SongInfo **out, int *cnt) {
    (void)loc;
    *out=NULL; *cnt=0; if(!json)return -1;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) { yyjson_doc_free(doc); return -1; }

    yyjson_val *songs = jget_arr(root, "songs");
    if (!songs) { yyjson_val *r = jget_obj(root, "result"); if (r) songs = jget_arr(r, "songs"); }
    if (!songs) { yyjson_val *d = jget_obj(root, "data"); if (d) songs = jget_arr(d, "dailySongs"); }
    if (!songs) { yyjson_doc_free(doc); return -1; }

    size_t n = yyjson_arr_size(songs);
    if (n == 0) { yyjson_doc_free(doc); return -1; }

    *out = (SongInfo*)calloc(n, sizeof(SongInfo));
    yyjson_val *v;
    yyjson_arr_iter iter = yyjson_arr_iter_with(songs);
    int oi = 0;
    while ((v = yyjson_arr_iter_next(&iter)) && oi < (int)n) {
        if (yyjson_is_obj(v)) fill(&(*out)[oi], v);
        oi++;
    }
    *cnt = oi;
    yyjson_doc_free(doc);
    return oi > 0 ? 0 : -1;
}

/* ── Init ──────────────────────────────────────────── */
int netease_init(void) {
    char *n=run("%s account-name%s",CLI,STDERR_REDIRECT);
    if(!n){LOG_WARN("netease-cli not found");return -1;}
    if(n[0]&&strcmp(n,"未登录\n")!=0&&strcmp(n,"error\n")!=0){size_t l=strlen(n);if(l>0&&n[l-1]=='\n')n[l-1]=0;snprintf(g_name,sizeof(g_name),"%s",n);}
    free(n);LOG_INFO("netease ready");return 0;
}
void netease_shutdown(void) {}
const char* netease_account_name(void) { return g_name[0]?g_name:NULL; }

/* ── Search ────────────────────────────────────────── */
int netease_search(const char *kw, int l, int o, NSSearchResult *out) {
    (void)o;
    memset(out,0,sizeof(*out)); if(!kw)return -1;
    char *esc = shell_escape(kw);
    char *j=run("%s search %s%s",CLI,esc,STDERR_REDIRECT);
    free(esc);
    if(!j)return -1;

    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) { yyjson_doc_free(doc); return -1; }

    yyjson_val *r = jget_obj(root, "result");
    yyjson_val *songs = r ? jget_arr(r, "songs") : NULL;
    if (!songs) { yyjson_doc_free(doc); return 0; }

    size_t n = yyjson_arr_size(songs);
    int max = (l > 0 && (size_t)l < n) ? l : (int)n;
    if (max == 0) { yyjson_doc_free(doc); return 0; }

    out->songs = calloc((size_t)max, sizeof(NSSong));
    out->count = max;

    yyjson_arr_iter iter = yyjson_arr_iter_with(songs);
    yyjson_val *v;
    int oi = 0;
    while ((v = yyjson_arr_iter_next(&iter)) && oi < max) {
        if (!yyjson_is_obj(v)) continue;
        NSSong *r = &out->songs[oi]; oi++;

        int64_t sid = jget_sint64(v, "id"); char bid[32]; snprintf(bid, sizeof(bid), "%ld", (long)sid); r->id = strdup(bid);
        const char *nm  = jget_str(v, "name"); r->title = nm ? strdup(nm) : strdup("");

        /* artist from ar[0].name */
        yyjson_val *ar = jget_arr(v, "ar") ? jget_arr(v, "ar") : jget_arr(v, "artists");
        if (ar) {
            yyjson_val *first = yyjson_arr_get_first(ar);
            const char *an = first ? jget_str(first, "name") : NULL;
            r->artist = an ? strdup(an) : strdup("");
        } else r->artist = strdup("");

        /* album + cover from al */
        yyjson_val *al = jget_obj(v, "al") ? jget_obj(v, "al") : jget_obj(v, "album");
        if (al) {
            const char *an = jget_str(al, "name");
            r->album = an ? strdup(an) : strdup("");
            const char *pu = jget_str(al, "picUrl");
            r->cover_url = pu ? strdup(pu) : strdup("");
        } else { r->album = strdup(""); r->cover_url = strdup(""); }

        r->dur_ms = (int)jget_int(v, "dt");
        r->fee = (int)jget_int(v, "fee");
    }
    out->count = oi;
    yyjson_doc_free(doc);
    return 0;
}
void netease_search_free(NSSearchResult *r) {
    if(!r)return;
    for(int i=0;i<r->count;i++){free(r->songs[i].id);free(r->songs[i].title);free(r->songs[i].artist);free(r->songs[i].album);free(r->songs[i].cover_url);}
    free(r->songs); r->songs=NULL; r->count=0;
}

/* Search playlists (type=1000). Result items carry is_playlist=1 so the
   UI can open them as playlists instead of playing them as songs. */
int netease_search_playlists(const char *kw, SongInfo **out, int *count) {
    *out=NULL; *count=0; if(!kw)return -1;
    char *esc = shell_escape(kw);
    char *j=run("%s search-pl %s%s",CLI,esc,STDERR_REDIRECT);
    free(esc);
    if(!j)return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *r = root ? jget_obj(root, "result") : NULL;
    yyjson_val *pl = r ? jget_arr(r, "playlists") : NULL;
    if (!pl) { yyjson_doc_free(doc); return 0; }

    size_t n = yyjson_arr_size(pl);
    if (n == 0) { yyjson_doc_free(doc); return 0; }

    *out = calloc(n, sizeof(SongInfo));
    int oi = 0;
    yyjson_arr_iter iter = yyjson_arr_iter_with(pl);
    yyjson_val *v;
    while ((v = yyjson_arr_iter_next(&iter))) {
        if (!yyjson_is_obj(v)) continue;
        SongInfo *s = &(*out)[oi];
        memset(s,0,sizeof(*s));
        s->source    = strdup("netease");
        s->cover_url = strdup("");
        s->aux_label = strdup("歌单");
        s->is_playlist = 1;
        int64_t sid = jget_sint64(v, "id");
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%ld", (long)sid);
        s->id = strdup(id_str);
        const char *nm = jget_str(v, "name"); s->title = nm ? strdup(nm) : strdup("");
        oi++;
    }
    *count = oi;
    yyjson_doc_free(doc);
    return oi > 0 ? 0 : -1;
}

/* Check whether a song is playable (has a stream URL — no copyright /
   delisted songs return an empty url). */
int netease_check_music(const char *song_id, bool *playable) {
    *playable = false;
    char *esc = shell_escape(song_id);
    char *j = run("%s check-music %s%s", CLI, esc, STDERR_REDIRECT);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long code = root ? jget_int(root, "code") : 0;
    if (code == 200)
        *playable = jget_bool(root, "playable");
    yyjson_doc_free(doc);
    return code == 200 ? 0 : -1;
}

/* Recently played songs (api/play-record/song/list). Response shape:
   {"code":200,"data":{"list":[{"resourceType":"SONG","data":{...song...}}, ...]}}
   The song object sits in "data" (older API versions may use "song"). */
int netease_recent_songs(SongInfo **out, int *count) {
    *out = NULL; *count = 0;
    char *j = run("%s record-recent%s", CLI, STDERR_REDIRECT);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = root ? jget_obj(root, "data") : NULL;
    yyjson_val *list = data ? jget_arr(data, "list") : NULL;
    if (!list) { yyjson_doc_free(doc); return 0; }

    size_t n = yyjson_arr_size(list);
    if (n == 0) { yyjson_doc_free(doc); return 0; }

    *out = calloc(n, sizeof(SongInfo));
    int oi = 0;
    yyjson_arr_iter iter = yyjson_arr_iter_with(list);
    yyjson_val *v;
    while ((v = yyjson_arr_iter_next(&iter))) {
        if (!yyjson_is_obj(v)) continue;
        /* keep only song entries (skip albums/DJ/etc.) */
        const char *rtype = jget_str(v, "resourceType");
        if (rtype && strcmp(rtype, "SONG") != 0) continue;
        yyjson_val *song = jget_obj(v, "data");
        if (!song) song = jget_obj(v, "song");
        if (!song) song = v;  /* entry may be the song itself */
        if (!yyjson_is_obj(song)) continue;
        fill(&(*out)[oi], song);
        oi++;
    }
    *count = oi;
    yyjson_doc_free(doc);
    return oi > 0 ? 0 : -1;
}

/* Daily recommend playlists (weapi/v1/discovery/recommend/resource).
   Response key is "recommend": [{id, name, playCount, ...}] */
int netease_daily_playlists(SongInfo **out, int *count) {
    *out = NULL; *count = 0;
    char *j = run("%s recommend-resource%s", CLI, STDERR_REDIRECT);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long code = root ? jget_int(root, "code") : 0;
    yyjson_val *pl = root ? jget_arr(root, "recommend") : NULL;
    if (code != 200 || !pl) { yyjson_doc_free(doc); return -1; }

    size_t n = yyjson_arr_size(pl);
    if (n == 0) { yyjson_doc_free(doc); return 0; }

    *out = calloc(n, sizeof(SongInfo));
    int oi = 0;
    yyjson_arr_iter iter = yyjson_arr_iter_with(pl);
    yyjson_val *v;
    while ((v = yyjson_arr_iter_next(&iter))) {
        if (!yyjson_is_obj(v)) continue;
        SongInfo *s = &(*out)[oi];
        memset(s,0,sizeof(*s));
        s->source    = strdup("netease");
        s->cover_url = strdup("");
        s->aux_label = strdup("歌单");
        s->is_playlist = 1;
        int64_t sid = jget_sint64(v, "id");
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%ld", (long)sid);
        s->id = strdup(id_str);
        const char *nm = jget_str(v, "name"); s->title = nm ? strdup(nm) : strdup("");
        oi++;
    }
    *count = oi;
    yyjson_doc_free(doc);
    return oi > 0 ? 0 : -1;
}

/* ── Login QR ─────────────────────────────────────── */
int netease_qr_key(char *u, size_t usz, char *url, size_t usz2) {
    char *j=run("%s qr-key",CLI); if(!j){LOG_ERROR("netease-cli not found");return -1;}
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *uk  = root ? jget_str(root, "unikey") : NULL;
    const char *url2 = root ? jget_str(root, "url") : NULL;
    int rv = -1;
    if (uk && url2 && uk[0] && url2[0]) { snprintf(u, usz, "%s", uk); snprintf(url, usz2, "%s", url2); rv = 0; }
    else LOG_ERROR("qr-key failed");
    yyjson_doc_free(doc);
    return rv;
}

char* netease_qr_render(const char *url) {
    char *esc = shell_escape(url);
    char *r = run("%s qr-render %s%s",CLI,esc,STDERR_REDIRECT);
    free(esc);
    return r;
}

/* High-resolution QR PNG as base64 (for the kitty graphics renderer).
   Caller frees the returned string. */
char* netease_qr_image(const char *url) {
    char *esc = shell_escape(url);
    char *r = run("%s qr-image %s%s",CLI,esc,STDERR_REDIRECT);
    free(esc);
    if (r) {
        size_t l = strlen(r);
        while (l > 0 && (r[l-1] == '\n' || r[l-1] == '\r')) r[--l] = 0;
    }
    return r;
}

int netease_qr_poll(const char *uk) {
    char *esc = shell_escape(uk);
    char *j=run("%s qr-check %s%s",CLI,esc,STDERR_REDIRECT);
    free(esc);
    if(!j)return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long c = root ? jget_int(root, "code") : 0;
    yyjson_doc_free(doc);
    if(c==803){char*n=run("%s account-name%s",CLI,STDERR_REDIRECT);if(n){size_t l=strlen(n);if(l>0&&n[l-1]=='\n')n[l-1]=0;if(strcmp(n,"error")!=0&&strcmp(n,"未登录")!=0)snprintf(g_name,sizeof(g_name),"%s",n);free(n);}return 0;}
    if(c==800)return 2;
    if(c==802)return 3;
    return 1;
}
bool netease_is_logged_in(void) { return g_name[0]!=0; }

/* ── Login refresh ────────────────────────────────── */
int netease_login_refresh(void) {
    char *j = run("%s login-refresh%s", CLI, STDERR_REDIRECT);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long c = root ? jget_int(root, "code") : 0;
    yyjson_doc_free(doc);
    return c == 200 ? 0 : -1;
}

/* ── Logout: drop the cached cookie file ──────────── */
int netease_logout(void) {
    const char *home = getenv_utf8("HOME");
#ifdef _WIN32
    if (!home || !home[0]) home = getenv_utf8("USERPROFILE");
#endif
    if (!home || !home[0]) return -1;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.cache/netune/cookies.txt", home);
    if (remove_utf8(path) == 0) {
        g_name[0] = 0;
        return 0;
    }
    /* file already gone counts as logged out */
    return g_name[0] ? -1 : 0;
}

/* ── Like / subscribe / toplist ───────────────────── */
int netease_like_song(const char *song_id, bool like) {
    char *esc = shell_escape(song_id);
    char *j = run("%s like %s %s%s", CLI, esc, like ? "true" : "false", STDERR_REDIRECT);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long code = root ? jget_int(root, "code") : 0;
    yyjson_doc_free(doc);
    return code == 200 ? 0 : -1;
}

int netease_liked_check(const char *song_id, bool *liked) {
    char *esc = shell_escape(song_id);
    char *j = run("%s liked-check %s%s", CLI, esc, STDERR_REDIRECT);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long code = root ? jget_int(root, "code") : 0;
    if (code == 200) {
        yyjson_val *v = root ? yyjson_obj_get(root, "liked") : NULL;
        *liked = v ? yyjson_get_bool(v) : false;
    }
    yyjson_doc_free(doc);
    return code == 200 ? 0 : -1;
}

int netease_subscribe_playlist(const char *pl_id, bool sub) {
    char *esc = shell_escape(pl_id);
    char *j = run("%s subscribe %s %s%s", CLI, esc, sub ? "1" : "0", STDERR_REDIRECT);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long code = root ? jget_int(root, "code") : 0;
    yyjson_doc_free(doc);
    return code == 200 ? 0 : -1;
}

/* ── Playlist management ──────────────────────────── */
/* Generic response-code checker for the *-cli JSON wrappers */
static int cli_code_ok(char *j) {
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long code = root ? jget_int(root, "code") : 0;
    yyjson_doc_free(doc);
    return code == 200 ? 0 : -1;
}

int netease_track_add(const char *pl_id, const char *song_id) {
    char *e1 = shell_escape(pl_id);
    char *e2 = shell_escape(song_id);
    char *j = run("%s track-add %s %s%s", CLI, e1, e2, STDERR_REDIRECT);
    free(e1); free(e2);
    return cli_code_ok(j);
}

int netease_track_remove(const char *pl_id, const char *song_id) {
    char *e1 = shell_escape(pl_id);
    char *e2 = shell_escape(song_id);
    char *j = run("%s track-del %s %s%s", CLI, e1, e2, STDERR_REDIRECT);
    free(e1); free(e2);
    return cli_code_ok(j);
}

int netease_playlist_create(const char *name, char *new_id, size_t id_sz) {
    if (new_id && id_sz) new_id[0] = 0;
    char *esc = shell_escape(name);
    char *j = run("%s playlist-create %s%s", CLI, esc, STDERR_REDIRECT);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long code = root ? jget_int(root, "code") : 0;
    if (code == 200 && new_id && id_sz) {
        yyjson_val *body = root ? jget_obj(root, "body") : NULL;
        yyjson_val *pl = body ? jget_obj(body, "playlist") : NULL;
        int64_t pid = pl ? jget_sint64(pl, "id") : 0;
        if (pid > 0)
            snprintf(new_id, id_sz, "%ld", (long)pid);
    }
    yyjson_doc_free(doc);
    return code == 200 ? 0 : -1;
}

int netease_playlist_rename(const char *pl_id, const char *name) {
    char *e1 = shell_escape(pl_id);
    char *e2 = shell_escape(name);
    char *j = run("%s playlist-rename %s %s%s", CLI, e1, e2, STDERR_REDIRECT);
    free(e1); free(e2);
    return cli_code_ok(j);
}

int netease_playlist_delete(const char *pl_id) {
    char *esc = shell_escape(pl_id);
    char *j = run("%s playlist-delete %s%s", CLI, esc, STDERR_REDIRECT);
    free(esc);
    return cli_code_ok(j);
}

int netease_toplist(SongInfo **out, int *count) {
    char *j = run("%s toplist%s", CLI, STDERR_REDIRECT); if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) { *out=NULL; *count=0; return -1; }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *pl = jget_arr(root, "list");
    if (!pl) { yyjson_doc_free(doc); *out=NULL; *count=0; return -1; }

    size_t n = yyjson_arr_size(pl);
    if (n == 0) { yyjson_doc_free(doc); *out=NULL; *count=0; return -1; }

    *out = calloc(n, sizeof(SongInfo));
    int oi = 0;
    yyjson_arr_iter iter = yyjson_arr_iter_with(pl);
    yyjson_val *v;
    while ((v = yyjson_arr_iter_next(&iter))) {
        if (!yyjson_is_obj(v)) continue;
        SongInfo *s = &(*out)[oi];
        memset(s,0,sizeof(*s));
        s->source    = strdup("netease");
        s->cover_url = strdup("");
        s->aux_label = strdup("歌单");
        s->is_playlist = 1;
        int64_t sid = jget_sint64(v, "id");
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%ld", (long)sid);
        s->id = strdup(id_str);
        const char *nm = jget_str(v, "name"); s->title = nm ? strdup(nm) : strdup("");
        oi++;
    }
    *count = oi;
    yyjson_doc_free(doc);
    return oi > 0 ? 0 : -1;
}

/* ── Playlists ────────────────────────────────────── */
int netease_playlists(bool favorited, SongInfo **out, int *count) {
    char *j=run("%s playlists%s",CLI,STDERR_REDIRECT); if(!j)return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) { *out=NULL; *count=0; return -1; }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *pl = jget_arr(root, "playlists");
    if (!pl) { yyjson_doc_free(doc); *out=NULL; *count=0; return -1; }

    size_t n = yyjson_arr_size(pl);
    if (n == 0) { yyjson_doc_free(doc); *out=NULL; *count=0; return -1; }

    *out = calloc(n, sizeof(SongInfo));
    int oi = 0;
    yyjson_arr_iter iter = yyjson_arr_iter_with(pl);
    yyjson_val *v;
    while ((v = yyjson_arr_iter_next(&iter))) {
        if (!yyjson_is_obj(v)) continue;
        bool sub = jget_bool(v, "subscribed");
        if (sub != favorited) continue;
        SongInfo *s = &(*out)[oi];
        memset(s,0,sizeof(*s));
        s->source    = strdup("netease");
        s->cover_url = strdup("");
        s->aux_label = strdup("歌单");
        s->is_playlist = 1;
        int64_t sid = jget_sint64(v, "id");
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%ld", (long)sid);
        s->id = strdup(id_str);
        const char *nm  = jget_str(v, "name"); s->title = nm  ? strdup(nm)  : strdup("");
        oi++;
    }
    *count = oi;
    yyjson_doc_free(doc);
    return oi > 0 ? 0 : -1;
}

int netease_playlist_songs(const char *id, SongInfo **out, int *count) {
    char *esc = shell_escape(id);
    char *j=run("%s playlist-tracks %s%s",CLI,esc,STDERR_REDIRECT);
    free(esc);
    if(!j)return -1;
    int r = parselist(j, "songs", out, count);
    free(j); return r;
}

int netease_liked_songs(SongInfo **out, int *count) {
    char *j=run("%s liked%s",CLI,STDERR_REDIRECT); if(!j)return -1;
    int r = parselist(j, "songs", out, count);
    free(j); return r;
}

int netease_menu_songs(int type, int limit, SongInfo **out, int *count) {
    (void)limit;
    if (type == 0) {
        char *j = run("%s recommend-songs%s",CLI,STDERR_REDIRECT); if(!j) return -1;
        int r = parselist(j, "songs", out, count);
        free(j); return r;
    }
    if (type == 1) {
        /* personalized playlist recommendations (推荐歌单) */
        char *j=run("%s recommend-playlists%s",CLI,STDERR_REDIRECT); if(!j)return -1;
        yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
        free(j);
        if (!doc) { *out=NULL; *count=0; return -1; }
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *pl = jget_arr(root, "result");
        if (!pl) { yyjson_doc_free(doc); *out=NULL; *count=0; return -1; }

        size_t n = yyjson_arr_size(pl);
        if (n == 0) { yyjson_doc_free(doc); *out=NULL; *count=0; return -1; }

        *out = calloc(n, sizeof(SongInfo));
        int oi = 0;
        yyjson_arr_iter iter = yyjson_arr_iter_with(pl);
        yyjson_val *v;
        while ((v = yyjson_arr_iter_next(&iter))) {
            if (!yyjson_is_obj(v)) continue;
            SongInfo *s = &(*out)[oi];
            memset(s,0,sizeof(*s));
            s->source    = strdup("netease");
            s->cover_url = strdup("");
            s->aux_label = strdup("歌单");
        s->is_playlist = 1;
            int64_t sid = jget_sint64(v, "id");
            char id_str[32];
            snprintf(id_str, sizeof(id_str), "%ld", (long)sid);
            s->id = strdup(id_str);
            const char *nm  = jget_str(v, "name"); s->title = nm  ? strdup(nm)  : strdup("");
            oi++;
        }
        *count = oi;
        yyjson_doc_free(doc);
        return oi > 0 ? 0 : -1;
    }
    return -1;
}

/* ── Play URL ──────────────────────────────────────── */
/* Resolve the play quality from netease_quality (per-song override >
   global config, then verify against the cached source table, degrading
   down the ladder when the wanted tier has no source) and stream at it. */
int netease_play_url(const char *id, char *url, size_t sz) {
    char *lvl = nq_resolve_level(id);
    if (!lvl) { if (sz > 0) url[0] = 0; return -1; }
    int rc = netease_song_url(id, lvl, url, sz);
    free(lvl);
    return rc;
}

int netease_song_url(const char *id, const char *level, char *url, size_t sz) {
    if (!id || !url || sz == 0) return -1;
    const char *lvl = (level && level[0]) ? level : "standard";
    char *esc = shell_escape(id);
    char *j = run("%s song-url %s %s%s", CLI, esc, lvl, STDERR_REDIRECT);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) { if (sz > 0) url[0] = 0; return -1; }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = jget_arr(root, "data");
    int r = -1;
    if (data) {
        yyjson_val *first = yyjson_arr_get_first(data);
        if (first && yyjson_is_obj(first)) {
            /* item code gates the download: 200 = ok, -120 = no download
               permission (VIP-gated), -110 = track unavailable, -103 = no
               permission. url is null for all of these, but check the code
               explicitly so a denied tier is never treated as success. */
            long long icode = jget_int(first, "code");
            const char *u = jget_str(first, "url");
            if (icode == 200 && u && u[0]) { snprintf(url, sz, "%s", u); r = 0; }
        }
    }
    yyjson_doc_free(doc);
    if (r != 0 && sz > 0) url[0] = 0;
    return r;
}

/* ── Download ───────────────────────────────────────── */
/* Quality ladder, highest first: used for fallback when the requested
   level is unavailable (VIP-gated, region-locked, ...). */
static const char *const kQualityLadder[] = {
    "jymaster", "sky", "jyeffect", "hires", "lossless",
    "exhigh", "higher", "standard"
};
#define KQUALITY_N  8

/* check-quality <id> <level> — single-level entitlement probe against the
   play endpoint (player/url/v1). *granted=1 only when the server would
   hand out that exact level (reason "ok"); anything else (free_trial,
   denied, no_url, no_data) means the level isn't downloadable. */
int netease_check_quality(const char *id, const char *level, int *granted) {
    if (!id || !level || !granted) return -1;
    char *esc = shell_escape(id);
    char *esc_lv = shell_escape(level);
    char *j = run("%s check-quality %s %s%s", CLI, esc, esc_lv, STDERR_REDIRECT);
    free(esc); free(esc_lv);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    int rv = -1;
    if (root && yyjson_is_obj(root)) {
        *granted = jget_bool(root, "granted") ? 1 : 0;
        rv = 0;
    }
    yyjson_doc_free(doc);
    return rv;
}

/* song-music-quality <id> — authoritative per-tier source probe. Returns a
   bitmask of which quality tiers the track actually has a file for, in
   download-picker order high→low (jymaster=bit0 … standard=bit7). A tier
   with no source simply has its bit clear — this is what tells the download
   picker that a "lossless" download would silently come back as 320k mp3.
   0 = ok. */
#define NQ_JYMASTER  (1u << 0)
#define NQ_SKY       (1u << 1)
#define NQ_JYEFFECT  (1u << 2)
#define NQ_HIRES     (1u << 3)
#define NQ_LOSSLESS  (1u << 4)
#define NQ_EXHIGH    (1u << 5)
#define NQ_HIGHER    (1u << 6)
#define NQ_STANDARD  (1u << 7)
#define NQ_LEVELS    8
int netease_song_music_quality(const char *id, unsigned *mask_out,
                               int *br_out /* [8], high→low, 0 = no source */) {
    if (!id || !mask_out) return -1;
    if (br_out)
        for (int i = 0; i < NQ_LEVELS; i++) br_out[i] = 0;
    char *esc = shell_escape(id);
    char *j = run("%s song-music-quality %s%s", CLI, esc, STDERR_REDIRECT);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    int rv = -1;
    unsigned mask = 0;
    if (root && yyjson_is_obj(root)) {
        yyjson_val *data = jget_obj(root, "data");
        if (data && yyjson_is_obj(data)) {
            static const struct { const char *k; unsigned bit; } tier[] = {
                { "jm", NQ_JYMASTER }, { "sk", NQ_SKY },
                { "je", NQ_JYEFFECT }, { "hr", NQ_HIRES },
                { "sq", NQ_LOSSLESS }, { "h", NQ_EXHIGH },
                { "m", NQ_HIGHER },    { "l", NQ_STANDARD }
            };
            for (size_t i = 0; i < sizeof(tier)/sizeof(tier[0]); i++) {
                yyjson_val *v = yyjson_obj_get(data, tier[i].k);
                if (v && yyjson_is_obj(v)) {
                    mask |= tier[i].bit;
                    if (br_out) {
                        yyjson_val *b = yyjson_obj_get(v, "br");
                        if (b && yyjson_is_num(b))
                            br_out[i] = (int)yyjson_get_int(b);
                    }
                }
            }
            *mask_out = mask;
            rv = 0;
        }
    }
    yyjson_doc_free(doc);
    return rv;
}

/* song-download-url <id> <level> — the OFFICIAL download endpoint
   (weapi/song/enhance/download/url/v1). Unlike the play URL this channel
   can serve up to Hi-Res even for free tracks, but VIP-gated levels come
   back denied there. Returns the URL (0 = ok) or -1. */
int netease_download_url(const char *id, const char *level, char *url, size_t sz) {
    if (!id || !url || sz == 0) return -1;
    const char *lvl = (level && level[0]) ? level : "standard";
    char *esc = shell_escape(id);
    char *esc_lv = shell_escape(lvl);
    char *j = run("%s song-download-url %s %s%s", CLI, esc, esc_lv, STDERR_REDIRECT);
    free(esc); free(esc_lv);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) { if (sz > 0) url[0] = 0; return -1; }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = jget_arr(root, "data");
    int r = -1;
    if (data) {
        yyjson_val *first = yyjson_arr_get_first(data);
        if (first && yyjson_is_obj(first)) {
            const char *u = jget_str(first, "url");
            if (u && u[0]) { snprintf(url, sz, "%s", u); r = 0; }
        }
    }
    yyjson_doc_free(doc);
    if (r != 0 && sz > 0) url[0] = 0;
    return r;
}

/* song-owned <id> <level> — resolve whether the track is purchased (owned).
   The download endpoint returns per-item `payed` (1 = owned, 0 = not) plus
   `code` (-120 = no download permission, 200 = ok). Free tracks report
   payed=0 but download fine; ownership only gates VIP/paid tracks.
   Returns: 1 = owned, 0 = not owned, -1 = probe failed / unavailable. */
int netease_song_owned(const char *id, const char *level) {
    if (!id) return -1;
    const char *lvl = (level && level[0]) ? level : "lossless";
    char *esc = shell_escape(id);
    char *esc_lv = shell_escape(lvl);
    char *j = run("%s song-download-url %s %s%s", CLI, esc, esc_lv, STDERR_REDIRECT);
    free(esc); free(esc_lv);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    int r = -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = root ? jget_arr(root, "data") : NULL;
    if (data) {
        yyjson_val *first = yyjson_arr_get_first(data);
        if (first && yyjson_is_obj(first)) {
            long long payed = jget_int(first, "payed");
            long long code  = jget_int(first, "code");
            if (payed == 1)      r = 1;
            else if (code == 200) r = 1;   /* free tier downloadable */
            else if (code == -120) r = 0;  /* no permission = not owned */
            else r = -1;                   /* -110 delisted / other */
        }
    }
    yyjson_doc_free(doc);
    return r;
}

/* ── Purchased-track list (api/single/mybought/song/list) ── */
static char *g_purchased_ids[1024];
static int   g_purchased_n = 0;
static int   g_purchased_loaded = 0;  /* 0 = not fetched, 1 = fetched ok, -1 = failed */

/* Refresh the cached purchased-track id list. 0 = ok, -1 = failed. */
int netease_purchased_refresh(void) {
    for (int i = 0; i < g_purchased_n; i++) { free(g_purchased_ids[i]); g_purchased_ids[i] = NULL; }
    g_purchased_n = 0;
    g_purchased_loaded = -1;

    /* paginate with limit=100 until hasMore is false */
    char *j = run("%s song-purchased 100 0%s", CLI, STDERR_REDIRECT);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = root ? jget_obj(root, "data") : NULL;
    if (!data || !yyjson_is_obj(data)) { yyjson_doc_free(doc); return -1; }

    int ok = 0;
    yyjson_val *list = jget_arr(data, "list");
    if (list) {
        size_t n = yyjson_arr_size(list);
        for (size_t i = 0; i < n; i++) {
            yyjson_val *it = yyjson_arr_get(list, i);
            if (!it || !yyjson_is_obj(it)) continue;
            long long sid = jget_sint64(it, "songId");
            if (sid <= 0) continue;
            char buf[32];
            snprintf(buf, sizeof buf, "%lld", sid);
            if (g_purchased_n < (int)(sizeof(g_purchased_ids)/sizeof(g_purchased_ids[0]))) {
                g_purchased_ids[g_purchased_n] = strdup(buf);
                g_purchased_n++;
            }
        }
        ok = 1;
    }
    /* hasMore handling: if there are more pages, keep fetching (cap safety) */
    long long has_more = jget_int(data, "hasMore");
    if (has_more && g_purchased_n < 512) {
        /* fetch page 1.. until exhausted (simple loop; typically 1 page) */
        for (int page = 1; page < 32 && g_purchased_n < 512; page++) {
            char cmd[128];
            snprintf(cmd, sizeof cmd, "%s song-purchased 100 %d%s", CLI, page * 100, STDERR_REDIRECT);
            char *j2 = run("%s", cmd);
            if (!j2) break;
            yyjson_doc *d2 = yyjson_read(j2, strlen(j2), 0);
            free(j2);
            if (!d2) break;
            yyjson_val *r2 = yyjson_doc_get_root(d2);
            yyjson_val *d2d = r2 ? jget_obj(r2, "data") : NULL;
            if (!d2d || !yyjson_is_obj(d2d)) { yyjson_doc_free(d2); break; }
            yyjson_val *l2 = jget_arr(d2d, "list");
            if (!l2) { yyjson_doc_free(d2); break; }
            size_t m = yyjson_arr_size(l2);
            if (m == 0) { yyjson_doc_free(d2); break; }
            for (size_t i = 0; i < m; i++) {
                yyjson_val *it = yyjson_arr_get(l2, i);
                if (!it || !yyjson_is_obj(it)) continue;
                long long sid = jget_sint64(it, "songId");
                if (sid <= 0) continue;
                char buf[32];
                snprintf(buf, sizeof buf, "%lld", sid);
                if (g_purchased_n < (int)(sizeof(g_purchased_ids)/sizeof(g_purchased_ids[0]))) {
                    g_purchased_ids[g_purchased_n] = strdup(buf);
                    g_purchased_n++;
                }
            }
            yyjson_doc_free(d2);
            if (jget_int(d2d, "hasMore") == 0) break;
        }
    }
    yyjson_doc_free(doc);
    if (ok) g_purchased_loaded = 1;
    return ok ? 0 : -1;
}

/* Whether a track is in the purchased list. 1 = purchased, 0 = not (list
   loaded), -1 = list not loaded yet / unknown. */
int netease_is_purchased(const char *song_id) {
    if (!song_id || !*song_id) return -1;
    if (!g_purchased_loaded) {
        if (netease_purchased_refresh() != 0)
            return -1;
    }
    for (int i = 0; i < g_purchased_n; i++) {
        if (g_purchased_ids[i] && strcmp(g_purchased_ids[i], song_id) == 0)
            return 1;
    }
    return g_purchased_loaded == 1 ? 0 : -1;
}

char* netease_download_song(const char *id, const char *level,
                            const char *title, char *used_level,
                            size_t used_sz) {
    if (!id) return NULL;

    /* Official download endpoint applies to the high tiers; the lower
       tiers (standard/higher/exhigh) are served by the same stream the
       player uses, so grab those from the play URL. */
    static const char *const kDownloadApiLevels[] = {
        "jymaster", "sky", "jyeffect", "hires", "lossless", NULL
    };

    const char *want = (level && level[0]) ? level : "standard";
    int start = 7;  /* index of "standard" */
    for (int i = 0; i < KQUALITY_N; i++) {
        if (strcmp(kQualityLadder[i], want) == 0) { start = i; break; }
    }

    char dl_url[4096];
    const char *used = NULL;
    for (int i = start; i < KQUALITY_N; i++) {
        const char *lvl = kQualityLadder[i];
        int via_dl = 0;
        for (int k = 0; kDownloadApiLevels[k]; k++) {
            if (strcmp(kDownloadApiLevels[k], lvl) == 0) { via_dl = 1; break; }
        }
        int ok = via_dl
            ? (netease_download_url(id, lvl, dl_url, sizeof(dl_url)) == 0)
            : (netease_song_url(id, lvl, dl_url, sizeof(dl_url)) == 0);
        if (ok) { used = lvl; break; }
    }
    if (!used) return NULL;
    if (used_level && used_sz > 0)
        snprintf(used_level, used_sz, "%s", used);

    /* Download dir: netune_data_root()/downloads */
    const char *root = netune_data_root();
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s" PATH_SEP "downloads", root);

    /* File extension: trust what the URL actually serves. The streaming
       endpoint may return a lower-encoded file than the requested level
       (e.g. "hires" delivered as 320k mp3), so guess from the URL path
       before the query string, falling back to the level mapping. */
    const char *ext = (strcmp(used, "jymaster") == 0 ||
                       strcmp(used, "sky") == 0 ||
                       strcmp(used, "jyeffect") == 0 ||
                       strcmp(used, "hires") == 0 ||
                       strcmp(used, "lossless") == 0) ? "flac" : "mp3";
    const char *q = strchr(dl_url, '?');
    size_t ulen = q ? (size_t)(q - dl_url) : strlen(dl_url);
    const char *p = dl_url + ulen;
    while (p > dl_url && p[-1] != '.') p--;
    if (p > dl_url && ulen - (p - dl_url) >= 3) {
        if (strncasecmp(p, "flac", 4) == 0)      ext = "flac";
        else if (strncasecmp(p, "m4a", 3) == 0)  ext = "m4a";
        else if (strncasecmp(p, "wav", 3) == 0)  ext = "wav";
        else                                     ext = "mp3";
    }
    const char *base = (title && title[0]) ? title : id;
    char sane[256];
    size_t k = 0;
    for (size_t i = 0; base[i] && k < sizeof(sane) - 8; i++) {
        char c = base[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
        sane[k++] = c;
    }
    /* dir (≤1024) + "/" + sane (≤247) + "." + ext (≤4) can exceed 1024.
       Trim sane so the joined path fits. */
    char path[1024];
    {
        size_t dir_len  = strlen(dir);
        size_t ext_len  = strlen(ext);
        size_t budget   = sizeof(path) - dir_len - ext_len - 2; /* sep + dot + NUL */
        if (budget > 247) budget = 247;                       /* sane[] cap */
        else if (dir_len + ext_len + 2 >= sizeof(path)) budget = 0;
        if (k > budget) k = budget;
        sane[k] = 0;
    }
    int pw = snprintf(path, sizeof(path), "%s" PATH_SEP "%s.%s", dir, sane, ext);
    if (pw < 0 || pw >= (int)sizeof(path)) {
        LOG_ERROR("download path too long, aborting: %s", dir);
        return NULL;
    }

    /* netune_ensure_dir() creates the PARENT directory of the given path,
       so pass the file path itself to create netune_data_root()/downloads
       (passing the dir would leave the leaf "downloads" uncreated and the
       curl write below would fail). */
    netune_ensure_dir(path);

    remove_utf8(path);
    char *esc_url = shell_escape_cmd(dl_url);
    char *esc_path = shell_escape_cmd(path);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "curl -sL --max-time 120 %s -o %s", esc_url, esc_path);
    free(esc_url);
    free(esc_path);
    int rc = system(cmd);
    if (rc != 0) { remove_utf8(path); return NULL; }
    return strdup(path);
}

/* ── VIP status ─────────────────────────────────────── */
/* Resolve the account's effective VIP entitlement for download gating.
   Mirrors the client: redplus.vipCode non-zero & unexpired → SVIP;
   else musicPackage.vipCode non-zero & unexpired → black-vinyl VIP;
   else no VIP. Returns 0 = none, 1 = black-vinyl VIP, 2 = SVIP, -1 = error. */
int netease_vip_level(void) {
    char *j = run("%s vip-info%s", CLI, STDERR_REDIRECT);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    int level = -1;
    if (root && yyjson_is_obj(root)) {
        yyjson_val *data = jget_obj(root, "data");
        if (data && yyjson_is_obj(data)) {
            long long now_ms = (long long)(time(NULL)) * 1000;
            level = 0;
            /* redplus (黑胶 SVIP): only count a non-zero, unexpired expiry */
            yyjson_val *rp = jget_obj(data, "redplus");
            if (rp) {
                long long code = jget_int(rp, "vipCode");
                long long exp  = jget_int(rp, "expireTime");
                if (code > 0 && exp > now_ms)
                    level = 2;
            }
            if (level < 2) {
                /* musicPackage (黑胶 VIP): only count an unexpired expiry */
                yyjson_val *mp = jget_obj(data, "musicPackage");
                if (mp) {
                    long long code = jget_int(mp, "vipCode");
                    long long exp  = jget_int(mp, "expireTime");
                    if (code > 0 && exp > now_ms)
                        level = 1;
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return level;
}

/* ── Lyrics ──────────────────────────────────────────── */
int netease_lyric(const char *song_id, char **buf) {
    if (!song_id || !buf) return -1;
    char *esc = shell_escape(song_id);
    char *j = run("%s lyric %s", CLI, esc);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    int rv = -1;
    if (root) {
        const char *lyric = jget_str(root, "lrc");
        /* Netease sometimes returns "lrc" as an object with a "lyric" field */
        if (!lyric) {
            yyjson_val *lrc = jget_obj(root, "lrc");
            if (lrc) lyric = jget_str(lrc, "lyric");
        }
        /* fallback: try "lyric" directly */
        if (!lyric) lyric = jget_str(root, "lyric");
        if (lyric && lyric[0]) {
            *buf = strdup(lyric);
            rv = 0;
        } else {
            long long code = jget_int(root, "code");
            if (code != 200) LOG_WARN("netease lyric api returned code=%lld", code);
        }
    }
    yyjson_doc_free(doc);
    return rv;
}

/* ── Song detail ────────────────────────────────────── */
void song_detail_free(SongDetail *d) {
    if (!d) return;
    free(d->title);   d->title   = NULL;
    free(d->artist);  d->artist  = NULL;
    free(d->album);   d->album   = NULL;
    free(d->publish); d->publish = NULL;
    free(d->cover_url); d->cover_url = NULL;
}

int netease_song_detail(const char *song_id, SongDetail *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->pop = -1;
    char *esc = shell_escape(song_id);
    char *j = run("%s song-detail %s%s", CLI, esc, STDERR_REDIRECT);
    free(esc);
    if (!j) return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *songs = root ? jget_arr(root, "songs") : NULL;
    if (!songs || yyjson_arr_size(songs) < 1) {
        yyjson_doc_free(doc);
        return -1;
    }
    yyjson_val *v = yyjson_arr_get_first(songs);
    const char *nm  = jget_str(v, "name");   out->title = nm  ? strdup(nm) : NULL;
    yyjson_val *al  = jget_obj(v, "al");
    const char *an  = al ? jget_str(al, "name") : NULL;
    out->album = an ? strdup(an) : NULL;
    const char *al_pic = al ? jget_str(al, "picUrl") : NULL;
    out->cover_url = al_pic ? strdup(al_pic) : NULL;

    /* artists joined with " / " */
    yyjson_val *ar = jget_arr(v, "ar");
    if (ar && yyjson_arr_size(ar) > 0) {
        yyjson_arr_iter iter = yyjson_arr_iter_with(ar);
        yyjson_val *a;
        size_t len = 0;
        while ((a = yyjson_arr_iter_next(&iter))) {
            const char *n = jget_str(a, "name");
            if (n) len += strlen(n) + 3;
        }
        char *buf = malloc(len + 1);
        buf[0] = 0;
        iter = yyjson_arr_iter_with(ar);
        bool first = true;
        while ((a = yyjson_arr_iter_next(&iter))) {
            const char *n = jget_str(a, "name");
            if (!n) continue;
            if (!first) strcat(buf, " / ");
            strcat(buf, n);
            first = false;
        }
        out->artist = buf;
    }
    out->duration_sec = (int)(jget_sint64(v, "dt") / 1000);
    out->fee = (int)jget_int(v, "fee");
    yyjson_val *pv = yyjson_obj_get(v, "pop");
    out->pop = pv ? (int)yyjson_get_real(pv) : -1;

    /* publish time (ms epoch) → local YYYY-MM-DD */
    int64_t pt = jget_sint64(v, "publishTime");
    if (pt > 0) {
        time_t t = (time_t)(pt / 1000);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
        out->publish = strdup(buf);
    }

    yyjson_doc_free(doc);
    return 0;
}
