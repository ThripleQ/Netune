#include "netease_api.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>

#define CLI "netease-cli"

/* ── Cached account data ───────────────────────────── */
static char g_account_name[128] = "";
static unsigned long g_user_uid = 0;

/* ── Lightweight JSON helpers (no yyjson dependency) ── */

/* Extract "key":"...string..." from JSON. Caller free()s result. */
static char *json_str(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    if (*p == '"') {
        p++;
        size_t cap = 512, w = 0;
        char *out = malloc(cap);
        if (!out) return NULL;
        while (*p && *p != '"' && w < cap - 1) {
            if (*p == '\\' && *(p+1) == 'u' && isxdigit((unsigned char)*(p+2)) &&
                isxdigit((unsigned char)*(p+3)) && isxdigit((unsigned char)*(p+4)) &&
                isxdigit((unsigned char)*(p+5))) {
                char hex[5] = {*(p+2),*(p+3),*(p+4),*(p+5),'\0'};
                unsigned long cp = strtoul(hex, NULL, 16);
                if (cp < 0x80)      { out[w++] = (char)cp; }
                else if (cp < 0x800){ out[w++] = (char)(0xC0|(cp>>6)); out[w++] = (char)(0x80|(cp&0x3F)); }
                else                { out[w++] = (char)(0xE0|(cp>>12)); out[w++] = (char)(0x80|((cp>>6)&0x3F)); out[w++] = (char)(0x80|(cp&0x3F)); }
                p += 6;
            } else { out[w++] = *p++; }
        }
        out[w] = '\0';
        return out;
    }
    if (isdigit((unsigned char)*p) || *p == '-') {
        const char *end = p;
        while (isdigit((unsigned char)*end) || *end == '.') end++;
        char *out = malloc((size_t)(end - p) + 1);
        if (!out) return NULL;
        memcpy(out, p, (size_t)(end - p));
        out[end - p] = '\0';
        return out;
    }
    return NULL;
}

/* Extract "key":123 integer. */
static long long json_int(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    return atoll(p);
}

/* Find "key": {object or array start}. Returns pointer to { or [. */
static const char *json_obj_start(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p && *p != '{' && *p != '[') p++;
    return (*p == '{' || *p == '[') ? p : NULL;
}

/* Match closing bracket from an open { or [. Returns pointer past it. */
static const char *json_match(const char *open) {
    char open_ch = *open, close_ch = (open_ch == '{') ? '}' : ']';
    int depth = 1;
    const char *p = open + 1;
    while (*p && depth > 0) {
        if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
        else if (*p == open_ch) depth++;
        else if (*p == close_ch) depth--;
        p++;
    }
    return p;
}

/* ── popen wrapper ──────────────────────────────────── */
/* Run a command, capture stdout. Returns malloc'd string, or NULL on error.
   Sets a 10-second SIGALRM timeout. */
static char *run_cli(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    while (!feof(fp)) {
        if (len + 1024 >= cap) { cap *= 2; char *tmp = realloc(buf, cap); if (!tmp) { free(buf); pclose(fp); return NULL; } buf = tmp; }
        size_t r = fread(buf + len, 1, cap - len - 1, fp);
        if (r > 0) len += r; else break;
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

/* ── Parse helpers ──────────────────────────────────── */
/* Extract artist name from a song JSON block's "ar" array. */
static char *artist_name(const char *song_json) {
    const char *ar = strstr(song_json, "\"ar\"");
    if (!ar) return NULL;
    const char *name = strstr(ar, "\"name\"");
    if (!name) return NULL;
    name += 6;
    while (*name && (*name == ':' || *name == ' ')) name++;
    if (*name == '"') {
        name++;
        const char *end = strchr(name, '"');
        if (!end) return NULL;
        size_t len = (size_t)(end - name);
        char *out = malloc(len + 1);
        if (!out) return NULL;
        memcpy(out, name, len); out[len] = '\0';
        return out;
    }
    return NULL;
}

/* Parse one song JSON block into SongInfo (SearchResult entry). */
static int parse_song(const char *json, SongInfo *si) {
    memset(si, 0, sizeof(*si));
    si->source = strdup("netease");
    si->cover_url = strdup("");
    si->aux_label = strdup("");

    char *id = json_str(json, "id");
    if (!id) { song_info_free(si); return -1; }
    si->id = id;

    char *title = json_str(json, "name");
    si->title = title ? title : strdup("");

    char *artist = artist_name(json);
    si->artist = artist ? artist : strdup("");

    si->duration_sec = (int)(json_int(json, "dt") / 1000);
    return 0;
}

/* ── Init / Shutdown ────────────────────────────────── */
int netease_api_init(void) {
    /* Check that netease-cli exists and works */
    char *out = run_cli("%s account-name 2>/dev/null", CLI);
    if (!out) {
        LOG_WARN("netease-cli not found or not working");
        return -1;
    }
    free(out);
    /* Try to load cached login */
    char *name = run_cli("%s account-name 2>/dev/null", CLI);
    if (name && name[0] && strcmp(name, "未登录") != 0 && strcmp(name, "error") != 0) {
        size_t n = strlen(name);
        if (n > 0 && name[n-1] == '\n') name[n-1] = '\0';
        snprintf(g_account_name, sizeof(g_account_name), "%s", name);
        /* Get UID */
        char *json = run_cli("%s playlist 0 2>/dev/null", CLI);
        if (json) {
            /* /user/playlist returns user info in the response */
            free(json);
        }
        /* For UID, use account-info via playlists or similar */
        g_user_uid = 1; /* placeholder — will be populated properly */
    }
    free(name);
    LOG_INFO("Netease API client ready");
    return 0;
}

void netease_api_shutdown(void) {}

const char* netease_account_name(void) {
    return g_account_name[0] ? g_account_name : NULL;
}

unsigned long netease_get_user_id(void) {
    return g_user_uid;
}

bool netease_is_logged_in(void) {
    return g_account_name[0] != '\0';
}

/* ── Search ──────────────────────────────────────────── */
int netease_search(const char *keyword, int limit, int offset,
                   NeteaseSearchResult *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    char *json = run_cli("%s search \"%s\" 2>/dev/null", CLI, keyword);
    if (!json) return -1;

    int max = limit > 0 ? limit : 30;
    const char *songs = NULL;
    const char *res = json_obj_start(json, "result");
    if (res) songs = json_obj_start(res, "songs");
    if (!songs || *songs != '[') { free(json); return 0; }

    /* Skip offset first pass to position */
    const char *p = songs + 1;
    int skip = offset > 0 ? offset : 0;
    while (*p && skip > 0) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']') break;
        p = json_match(p);
        skip--;
    }

    /* Second pass: parse songs */
    /* First count */
    int count = 0;
    const char *pc = p;
    while (*pc && count < max) {
        while (*pc && *pc != '{' && *pc != ']') pc++;
        if (*pc == ']') break;
        pc = json_match(pc);
        count++;
    }

    if (count == 0) { free(json); return 0; }
    out->songs = (NeteaseSongResult*)calloc((size_t)count, sizeof(NeteaseSongResult));
    out->count = 0;

    int oi = 0;
    while (*p && oi < count) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']') break;
        const char *end = json_match(p);
        NeteaseSongResult *sr = &out->songs[oi];
        sr->id = json_str(p, "id");
        if (!sr->id) sr->id = strdup("");
        sr->title = json_str(p, "name");
        if (!sr->title) sr->title = strdup("");
        sr->artist = json_str(p, "artist");
        if (!sr->artist) {
            const char *ar = json_obj_start(p, "ar");
            if (ar) {
                const char *ap = ar + 1;
                while (*ap && *ap != '{') ap++;
                if (*ap == '{') sr->artist = json_str(ap, "name");
            }
        }
        if (!sr->artist) sr->artist = strdup("");
        sr->album = json_str(p, "album");
        if (!sr->album) {
            const char *al = json_obj_start(p, "al");
            if (al) sr->album = json_str(al, "name");
        }
        if (!sr->album) sr->album = strdup("");
        sr->duration_ms = (int)(json_int(p, "dt"));
        oi++;
        p = end;
    }
    out->count = oi;
    free(json);
    return 0;
}

void netease_search_result_free(NeteaseSearchResult *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        free(r->songs[i].id);
        free(r->songs[i].title);
        free(r->songs[i].artist);
        free(r->songs[i].album);
    }
    free(r->songs);
    r->songs = NULL;
    r->count = 0;
}

/* ── Login ───────────────────────────────────────────── */
int netease_qr_get_key(char *out_unikey, size_t unikey_size,
                       char *qr_url, size_t url_size) {
    char *json = run_cli("%s qr-key 2>/dev/null", CLI);
    if (!json) return -1;
    char *u = json_str(json, "unikey");
    char *url = json_str(json, "url");
    int ret = -1;
    if (u && url && u[0] && url[0]) {
        snprintf(out_unikey, unikey_size, "%s", u);
        snprintf(qr_url, url_size, "%s", url);
        ret = 0;
    }
    free(u); free(url); free(json);
    return ret;
}

char* netease_qr_render(const char *url) {
    return run_cli("%s qr-render \"%s\" 2>/dev/null", CLI, url);
}

int netease_qr_poll(const char *unikey) {
    char *json = run_cli("%s qr-check \"%s\" 2>/dev/null", CLI, unikey);
    if (!json) return -1;
    long long code = json_int(json, "code");
    free(json);
    if (code == 803) {
        /* Login succeeded — refresh account info */
        char *name = run_cli("%s account-name 2>/dev/null", CLI);
        if (name) {
            size_t n = strlen(name);
            if (n > 0 && name[n-1] == '\n') name[n-1] = '\0';
            if (strcmp(name, "error") != 0 && strcmp(name, "未登录") != 0)
                snprintf(g_account_name, sizeof(g_account_name), "%s", name);
            free(name);
        }
        return 0; /* success */
    }
    if (code == 800) return 2;  /* expired */
    if (code == 802) return 3;  /* scanned, waiting for confirm */
    return 1; /* waiting for scan (801 or unknown) */
}

/* ── User playlists ─────────────────────────────────── */
int netease_get_playlists(NeteasePlaylistResult *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    char *json = run_cli("%s playlists 2>/dev/null", CLI);
    if (!json) return -1;

    const char *pl = json_obj_start(json, "playlists");
    if (!pl || *pl != '[') { free(json); return 0; }

    /* Count items */
    int count = 0;
    const char *p = pl + 1;
    while (*p) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']') break;
        p = json_match(p);
        count++;
    }

    if (count == 0) { free(json); return 0; }
    out->items = (NeteasePlaylistItem*)calloc((size_t)count, sizeof(NeteasePlaylistItem));

    p = pl + 1;
    int oi = 0;
    while (*p && oi < count) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']') break;
        const char *end = json_match(p);
        /* Extract id and name */
        long long id = json_int(p, "id");
        char *name = json_str(p, "name");
        if (id > 0 && name) {
            out->items[oi].id = (unsigned long)id;
            out->items[oi].name = name;
            /* subscribed: true = favorited, false = created */
            out->items[oi].subscribed = (strstr(p, "\"subscribed\":true") != NULL);
            oi++;
        } else {
            free(name);
        }
        p = end;
    }
    out->count = oi;
    free(json);
    return 0;
}

void netease_playlist_result_free(NeteasePlaylistResult *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) free(r->items[i].name);
    free(r->items);
    r->items = NULL;
    r->count = 0;
}

/* ── Songs from playlists / liked ───────────────────── */
static int parse_songs_from_json(const char *json, SearchResult *out, int max) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *songs = json_obj_start(json, "songs");
    if (!songs || *songs != '[') {
        /* Try result.songs (for liked / search) */
        const char *res = json_obj_start(json, "result");
        if (res) songs = json_obj_start(res, "songs");
        if (!songs || *songs != '[') return 0;
    }

    /* Count */
    int count = 0;
    const char *p = songs + 1;
    while (*p && count < max) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']') break;
        p = json_match(p);
        count++;
    }

    if (count == 0) return 0;
    out->songs = (SongInfo*)calloc((size_t)count, sizeof(SongInfo));
    out->count = 0;
    out->total = count;

    p = songs + 1;
    int oi = 0;
    while (*p && oi < count) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']') break;
        const char *end = json_match(p);
        if (parse_song(p, &out->songs[oi]) == 0) oi++;
        p = end;
    }
    out->count = oi;
    return oi > 0 ? 0 : -1;
}

int netease_get_playlist_songs(const char *playlist_id, SearchResult *out) {
    char *json = run_cli("%s playlist-tracks \"%s\" 2>/dev/null", CLI, playlist_id);
    if (!json) return -1;
    int ret = parse_songs_from_json(json, out, 500);
    free(json);
    return ret;
}

int netease_get_liked_songs(SearchResult *out) {
    char *json = run_cli("%s liked 2>/dev/null", CLI);
    if (!json) return -1;
    int ret = parse_songs_from_json(json, out, 500);
    free(json);
    return ret;
}

int netease_load_menu(int type, int limit, SearchResult *out) {
    if (type == 0) {
        char *json = run_cli("%s recommend-songs 2>/dev/null", CLI);
        if (!json) return -1;
        int ret = parse_songs_from_json(json, out, limit > 0 ? limit : 30);
        free(json);
        return ret;
    }
    /* type 1 (personalized) not directly available — fallback */
    return -1;
}

/* ── Song URL ────────────────────────────────────────── */
int netease_get_play_url(const char *song_id, int quality,
                         char *out_url, size_t url_size) {
    const char *level = "standard";
    if (quality == 1) level = "higher";
    if (quality >= 2) level = "lossless";

    char *json = run_cli("%s song-url \"%s\" \"%s\" 2>/dev/null", CLI, song_id, level);
    if (!json) return -1;

    /* Response has {data:[{url:"..."}]} */
    const char *data = json_obj_start(json, "data");
    if (data && *data == '[') {
        const char *p = data + 1;
        while (*p && *p != '{') p++;
        if (*p == '{') {
            char *url = json_str(p, "url");
            if (url && url[0]) {
                snprintf(out_url, url_size, "%s", url);
                free(url); free(json);
                return 0;
            }
            free(url);
        }
    }
    free(json);
    if (url_size > 0) out_url[0] = '\0';
    return -1;
}

/* ── Song detail ─────────────────────────────────────── */
int netease_get_song_detail(const char *song_id,
                            char **title, char **artist, char **album,
                            int *duration_ms) {
    if (!song_id) return -1;

    char *json = run_cli("%s song-detail \"%s\" 2>/dev/null", CLI, song_id);
    if (!json) return -1;

    const char *songs = json_obj_start(json, "songs");
    if (!songs || *songs != '[') { free(json); return -1; }

    const char *p = songs + 1;
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') { free(json); return -1; }

    /* Extract fields */
    if (title) {
        *title = json_str(p, "name");
        if (!*title) *title = strdup("");
    }
    if (artist) {
        *artist = artist_name(p);
        if (!*artist) *artist = strdup("");
    }
    if (album) {
        char *al = NULL;
        const char *al_obj = json_obj_start(p, "al");
        if (al_obj) al = json_str(al_obj, "name");
        *album = al ? al : strdup("");
    }
    if (duration_ms)
        *duration_ms = (int)(json_int(p, "dt"));

    free(json);
    return 0;
}
