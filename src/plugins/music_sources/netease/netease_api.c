#include "netease_api.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#define popen  _popen
#define pclose _pclose
#endif
#include <stdarg.h>
#include <yyjson.h>

#define CLI "netease-cli"
static char g_name[128] = "";

/* ── popen helper ─────────────────────────────────── */
static char *run(const char *fmt, ...) {
    char cmd[2048]; va_list ap;
    va_start(ap, fmt); vsnprintf(cmd, sizeof(cmd), fmt, ap); va_end(ap);
    FILE *fp = popen(cmd, "r"); if (!fp) return NULL;
    size_t cap = 8192, len = 0; char *b = malloc(cap); if (!b) { pclose(fp); return NULL; }
    while (!feof(fp)) {
        if (len+1024>=cap) { cap*=2; char*t=realloc(b,cap); if(!t){free(b);pclose(fp);return NULL;} b=t; }
        size_t r=fread(b+len,1,cap-len-1,fp); if(r>0)len+=r; else break;
    }
    b[len]=0; pclose(fp); return b;
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
/* Get first object from an array (for arrays of objects). */
static yyjson_val *jfirst_obj(yyjson_val *arr) {
    if (!arr || !yyjson_is_arr(arr)) return NULL;
    yyjson_val *v = yyjson_arr_get_first(arr);
    return (v && yyjson_is_obj(v)) ? v : NULL;
}

/* ── parse one song from a yyjson_val object ──────── */
static void fill(SongInfo *s, yyjson_val *song) {
    memset(s,0,sizeof(*s));
    s->source            = strdup("netease");
    s->aux_label         = strdup("");

    int64_t sid = jget_sint64(song, "id");
    char idbuf[32]; snprintf(idbuf, sizeof(idbuf), "%lld", (long long)sid);
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
    size_t idx; yyjson_val *v;
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
    char *n=run("%s account-name 2>/dev/null",CLI);
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
    char *j=run("%s search \"%s\" 2>/dev/null",CLI,kw); if(!j)return -1;

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

        int64_t sid = jget_sint64(v, "id"); char bid[32]; snprintf(bid, sizeof(bid), "%lld", (long long)sid); r->id = strdup(bid);
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
    if (rv != 0) LOG_ERROR("qr-key failed"); /* re-log outside the if-else to keep msg after doc free (or not, just a LOG) */
    return rv;
}

char* netease_qr_render(const char *url) { return run("%s qr-render \"%s\" 2>/dev/null",CLI,url); }

int netease_qr_poll(const char *uk) {
    char *j=run("%s qr-check \"%s\" 2>/dev/null",CLI,uk); if(!j)return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) return -1;
    yyjson_val *root = yyjson_doc_get_root(doc);
    long long c = root ? jget_int(root, "code") : 0;
    yyjson_doc_free(doc);
    if(c==803){char*n=run("%s account-name 2>/dev/null",CLI);if(n){size_t l=strlen(n);if(l>0&&n[l-1]=='\n')n[l-1]=0;if(strcmp(n,"error")!=0&&strcmp(n,"未登录")!=0)snprintf(g_name,sizeof(g_name),"%s",n);free(n);}return 0;}
    if(c==800)return 2;
    if(c==802)return 3;
    return 1;
}
bool netease_is_logged_in(void) { return g_name[0]!=0; }

/* ── Playlists ────────────────────────────────────── */
int netease_playlists(bool favorited, SongInfo **out, int *count) {
    char *j=run("%s playlists 2>/dev/null",CLI); if(!j)return -1;
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
        int64_t sid = jget_sint64(v, "id");
        char id_str[32];
        snprintf(id_str, sizeof(id_str), "%lld", sid);
        s->id = strdup(id_str);
        const char *nm  = jget_str(v, "name"); s->title = nm  ? strdup(nm)  : strdup("");
        oi++;
    }
    *count = oi;
    yyjson_doc_free(doc);
    return oi > 0 ? 0 : -1;
}

int netease_playlist_songs(const char *id, SongInfo **out, int *count) {
    char *j=run("%s playlist-tracks \"%s\" 2>/dev/null",CLI,id); if(!j)return -1;
    int r = parselist(j, "songs", out, count);
    free(j); return r;
}

int netease_liked_songs(SongInfo **out, int *count) {
    char *j=run("%s liked 2>/dev/null",CLI); if(!j)return -1;
    int r = parselist(j, "songs", out, count);
    free(j); return r;
}

int netease_menu_songs(int type, int limit, SongInfo **out, int *count) {
    (void)limit;
    if (type == 0) {
        char *j = run("%s recommend-songs 2>/dev/null",CLI); if(!j) return -1;
        int r = parselist(j, "songs", out, count);
        free(j); return r;
    }
    return -1;
}

/* ── Play URL ──────────────────────────────────────── */
int netease_play_url(const char *id, char *url, size_t sz) {
    const char *lvl = "standard";
    char *j = run("%s song-url \"%s\" %s 2>/dev/null",CLI,id,lvl); if(!j)return -1;
    yyjson_doc *doc = yyjson_read(j, strlen(j), 0);
    free(j);
    if (!doc) { if(sz>0)url[0]=0; return -1; }
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

/* ── Lyrics ──────────────────────────────────────────── */
int netease_lyric(const char *song_id, char **buf) {
    if (!song_id || !buf) return -1;
    char *j = run("%s lyric \"%s\"", CLI, song_id);
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

char* netease_download(const char *id, const char *url) {
    char path[256]; snprintf(path,sizeof(path),"/tmp/netune_%s.mp3",id);
    unlink(path);
    char cmd[3072]; snprintf(cmd,sizeof(cmd),"curl -sL --max-time 60 \"%s\" -o \"%s\"",url,path);
    int rc = system(cmd);
    if (rc != 0) { unlink(path); return NULL; }
    return strdup(path);
}
