#include "netease_api.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <time.h>
#include <curl/curl.h>
#include <yyjson.h>

/* ── Configuration ──────────────────────────────────── */
static char g_base_url[256] = "http://localhost:10000";
static pid_t g_server_pid = 0;

/* ── Cookie file path ───────────────────────────────── */
static char g_cookie_path[512] = {0};

/* Path to the Node.js API server */
#define SERVER_DIR  "/home/liu/Projects/netease-api-local/node_modules/NeteaseCloudMusicApi"
#define SERVER_PORT "10000"
static char g_cookie[16384] = {0};
static char g_account_name[128] = {0};
static CURL *g_curl = NULL;
static bool g_capture_cookies = false;  /* only capture during login flow */

/* ── CURL write callback ────────────────────────────── */
typedef struct { char *data; size_t len; size_t cap; } WriteBuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    WriteBuf *buf = (WriteBuf*)userdata;
    if (buf->len + total + 1 > buf->cap) {
        buf->cap = buf->cap ? buf->cap * 2 : 16384;
        buf->data = (char*)realloc(buf->data, buf->cap);
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* ── HTTP GET request ───────────────────────────────── */
static int api_get(const char *path, WriteBuf *out) {
    if (!g_curl) return -1;

    memset(out, 0, sizeof(*out));

    char url[1024];
    if (path[0] == '/')
        snprintf(url, sizeof(url), "%s%s", g_base_url, path);
    else
        snprintf(url, sizeof(url), "%s/%s", g_base_url, path);

    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(g_curl, CURLOPT_HTTPGET, 1L);
    /* Send stored cookies from g_cookie. After QR login (code 803), api_get()
       captures the MUSIC_U cookie from the response and updates g_cookie,
       so subsequent requests automatically carry it.
       This is safe now because 803 is correctly mapped to success (not 802),
       so the login flow stops before any cookie conflict can occur. */
    if (g_cookie[0])
        curl_easy_setopt(g_curl, CURLOPT_COOKIE, g_cookie);
    else
        curl_easy_setopt(g_curl, CURLOPT_COOKIE, NULL);

    /* Suppress curl output */
    curl_easy_setopt(g_curl, CURLOPT_VERBOSE, 0L);

    CURLcode res = curl_easy_perform(g_curl);
    if (res != CURLE_OK) {
        LOG_WARN("API request failed: %s", curl_easy_strerror(res));
        return -1;
    }

    /* Only capture cookies during login flow (g_capture_cookies==true).
       During normal API calls, CURLOPT_COOKIE handles sending g_cookie
       without needing to update it. Reading CURLINFO_COOKIELIST would
       return only engine-jar cookies (like NMTID), overwriting saved
       cookies like MUSIC_R_U. */
    if (g_capture_cookies) {
        struct curl_slist *cookies = NULL;
        curl_easy_getinfo(g_curl, CURLINFO_COOKIELIST, &cookies);
        if (cookies) {
            char buf[16384] = {0};
            struct curl_slist *nc = cookies;
            while (nc) {
                if (nc->data && nc->data[0] != '#') {
                    int tab_count = 0;
                    const char *name_start = NULL, *value_start = NULL;
                    for (const char *p = nc->data; *p; p++) {
                        if (*p == '\t') {
                            tab_count++;
                            if (tab_count == 5) name_start = p + 1;
                            if (tab_count == 6 && value_start == NULL) value_start = p + 1;
                        }
                    }
                    if (name_start && value_start) {
                        if (buf[0]) { size_t bl = strlen(buf); strncat(buf, "; ", sizeof(buf) - bl - 1); }
                        size_t nlen = (size_t)(value_start - name_start - 1);
                        strncat(buf, name_start, nlen);
                        { size_t bl = strlen(buf); strncat(buf, "=", sizeof(buf) - bl - 1); }
                        { size_t bl = strlen(buf); strncat(buf, value_start, sizeof(buf) - bl - 1); }
                        size_t blen = strlen(buf);
                        if (blen > 0 && buf[blen-1] == '\t') buf[blen-1] = '\0';
                    }
                }
                nc = nc->next;
            }
            curl_slist_free_all(cookies);
            if (buf[0]) {
                LOG_INFO("Cookie capture: %.100s", buf);
                snprintf(g_cookie, sizeof(g_cookie), "%s", buf);
            }
        }
    }

    return 0;
}

/* ── Parse JSON response, check code == 200 ────────── */
static yyjson_doc* parse_ok(WriteBuf *buf) {
    yyjson_doc *doc = yyjson_read(buf->data, buf->len, 0);
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *code = yyjson_obj_get(root, "code");
    if (!code || yyjson_get_int(code) != 200) {
        yyjson_doc_free(doc);
        return NULL;
    }
    return doc;
}

/* ── Cookie persistence ─────────────────────────────── */
/* ── User ID (stored after login) ──────────────────── */
static unsigned long g_user_uid = 0;

static void cookie_set_path(void) {
    if (g_cookie_path[0]) return;
    const char *home = getenv("HOME");
    if (home)
        snprintf(g_cookie_path, sizeof(g_cookie_path), "%s/.cache/lmusic/netease_cookie.txt", home);
    else
        snprintf(g_cookie_path, sizeof(g_cookie_path), "/tmp/netease_cookie.txt");
}

static void cookie_save(void) {
    cookie_set_path();
    FILE *fp = fopen(g_cookie_path, "w");
    if (!fp) return;
    fprintf(fp, "%s\n", g_cookie);
    fprintf(fp, "%lu\n", g_user_uid);
    if (g_account_name[0])
        fprintf(fp, "%s\n", g_account_name);
    fclose(fp);
}

static void cookie_load(void) {
    cookie_set_path();
    FILE *fp = fopen(g_cookie_path, "r");
    if (!fp) return;
    char buf[16384];
    /* cookie line (may be very long — MUSIC_U is 770 chars alone) */
    if (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
        snprintf(g_cookie, sizeof(g_cookie), "%s", buf);
    }
    /* uid line */
    if (fgets(buf, sizeof(buf), fp)) {
        g_user_uid = atol(buf);
    }
    /* nickname line */
    if (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
        snprintf(g_account_name, sizeof(g_account_name), "%s", buf);
    }
    fclose(fp);
    if (g_cookie[0])
        LOG_INFO("Netease cookie restored: %s (uid=%lu)", g_account_name, g_user_uid);
}

/* ── Server lifecycle ────────────────────────────────── */
/* Quick check if server is responding (silent — no warnings) */
static int server_alive(void) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    curl_easy_setopt(c, CURLOPT_URL, "http://localhost:10000");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);  /* discard response */
    WriteBuf dummy = {0};
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &dummy);
    CURLcode res = curl_easy_perform(c);
    if (dummy.data) free(dummy.data);
    curl_easy_cleanup(c);
    return (res == CURLE_OK) ? 0 : -1;
}

/* Start the Node.js API server as a child process */
/* Generate/persist device ID to avoid creating new anonymous devices on each login */
static void ensure_device_id(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.cache/lmusic/device_id.txt", home);
    FILE *fp = fopen(path, "r");
    if (fp) {
        char id[64] = {0};
        if (fgets(id, sizeof(id), fp)) {
            size_t n = strlen(id);
            if (n > 0 && id[n-1] == '\n') id[n-1] = '\0';
            setenv("DEVICE_ID", id, 1);
        }
        fclose(fp);
        return;
    }
    /* Generate a 40-char hex device ID */
    srand((unsigned)(time(NULL) ^ (long)getpid()));
    char id[64];
    for (int i = 0; i < 40; i++) {
        int r = rand() % 16;
        id[i] = r < 10 ? '0' + r : 'A' + r - 10;
    }
    id[40] = '\0';
    setenv("DEVICE_ID", id, 1);
    fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%s\n", id);
        fclose(fp);
    }
    LOG_INFO("Generated persistent device ID for API server");
}

static int start_server(void) {
    if (g_server_pid > 0) return 0; /* already started */

    ensure_device_id();

    pid_t pid = fork();
    if (pid < 0) {
        LOG_WARN("Failed to fork API server");
        return -1;
    }
    if (pid == 0) {
        /* Child process: auto-kill on parent death */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        /* Redirect output and exec the server */
        freopen("/tmp/netease-api.log", "a", stdout);
        freopen("/tmp/netease-api.log", "a", stderr);
        setenv("PORT", SERVER_PORT, 1);
        setenv("DEVICE_ID", getenv("DEVICE_ID"), 1);
        chdir(SERVER_DIR);
        execl("/usr/bin/node", "node", "app.js", NULL);
        _exit(1); /* only reached if exec fails */
    }

    g_server_pid = pid;
    LOG_INFO("API server started (PID %d)", pid);

    /* Wait for server to be ready (up to 10s) */
    for (int i = 0; i < 100; i++) {
        if (server_alive() == 0) {
            LOG_INFO("API server ready (%.1fs)", (i + 1) * 0.1);
            return 0;
        }
        usleep(100000); /* 100ms */
    }

    LOG_WARN("API server did not start within 10s");
    kill(g_server_pid, SIGTERM);
    waitpid(g_server_pid, NULL, 0);
    g_server_pid = 0;
    return -1;
}

unsigned long netease_get_user_id(void) {
    return g_user_uid;
}

/* ── User playlists ─────────────────────────────────── */
int netease_get_playlists(unsigned long uid, int mine_only, NeteasePlaylistResult *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (uid == 0) return -1;

    char path[256];
    snprintf(path, sizeof(path), "/user/playlist?uid=%lu&limit=100", uid);

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *playlists = yyjson_obj_get(root, "playlist");
    if (!playlists || !yyjson_is_arr(playlists)) {
        yyjson_doc_free(doc);
        return 0;
    }

    size_t n = yyjson_arr_size(playlists);
    out->items = (NeteasePlaylist*)calloc(n, sizeof(NeteasePlaylist));

    size_t idx, max;
    yyjson_val *pl;
    int oi = 0;
    yyjson_arr_foreach(playlists, idx, max, pl) {
        unsigned long pid  = 0;
        yyjson_val *v = yyjson_obj_get(pl, "id");
        if (v) pid = (unsigned long)yyjson_get_uint(v);

        v = yyjson_obj_get(pl, "name");
        const char *name = v ? yyjson_get_str(v) : "";

        int tc = 0;
        v = yyjson_obj_get(pl, "trackCount");
        if (v) tc = (int)yyjson_get_int(v);

        bool sub = false;
        v = yyjson_obj_get(pl, "subscribed");
        if (v) sub = yyjson_get_bool(v);

        if (mine_only == 1 && sub) continue;
        if (mine_only == 2 && !sub) continue;

        out->items[oi].id = pid;
        out->items[oi].name = strdup(name);
        out->items[oi].track_count = tc;
        out->items[oi].subscribed = sub;
        oi++;
    }

    out->count = oi;
    yyjson_doc_free(doc);
    return 0;
}

void netease_playlist_result_free(NeteasePlaylistResult *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++)
        free(r->items[i].name);
    free(r->items);
    r->items = NULL;
    r->count = 0;
}

/* ── Playlist songs ─────────────────────────────────── */
int netease_get_playlist_songs(unsigned long playlist_id, SearchResult *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    char path[512];
    snprintf(path, sizeof(path), "/playlist/detail?id=%lu", playlist_id);

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *pl = yyjson_obj_get(root, "playlist");
    yyjson_val *tracks = pl ? yyjson_obj_get(pl, "tracks") : NULL;
    if (!tracks) { yyjson_doc_free(doc); return 0; }

    size_t n = yyjson_arr_size(tracks);
    size_t limit = n > 100 ? 100 : n;
    out->songs = (SongInfo*)calloc(limit, sizeof(SongInfo));
    out->count = (int)limit;
    out->total = (int)n;

    size_t idx, max;
    yyjson_val *tr;
    int oi = 0;
    yyjson_arr_foreach(tracks, idx, max, tr) {
        if (oi >= (int)limit) break;
        SongInfo *si = &out->songs[oi++];
        memset(si, 0, sizeof(*si));
        yyjson_val *v;

        v = yyjson_obj_get(tr, "id");
        if (v) {
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "%llu", (unsigned long long)yyjson_get_uint(v));
            si->id = strdup(id_buf);
        }
        si->source = strdup("netease");

        v = yyjson_obj_get(tr, "name");
        si->title = v ? strdup(yyjson_get_str(v)) : strdup("");

        yyjson_val *artists = yyjson_obj_get(tr, "ar");
        if (!artists) artists = yyjson_obj_get(tr, "artists");
        if (artists && yyjson_arr_size(artists) > 0) {
            v = yyjson_obj_get(yyjson_arr_get(artists, 0), "name");
            si->artist = v ? strdup(yyjson_get_str(v)) : strdup("");
        } else {
            si->artist = strdup("");
        }

        v = yyjson_obj_get(tr, "dt");
        if (!v) v = yyjson_obj_get(tr, "duration");
        si->duration_sec = v ? (int)(yyjson_get_int(v) / 1000) : 0;
    }

    yyjson_doc_free(doc);
    return out->count > 0 ? 0 : -1;
}

/* ── Liked songs (红心歌单) ────────────────────────── */
int netease_get_liked_songs(unsigned long uid, SearchResult *out) {
    if (!out || uid == 0) return -1;
    memset(out, 0, sizeof(*out));

    char path[256];
    snprintf(path, sizeof(path), "/likelist?uid=%lu", uid);
    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *ids = yyjson_obj_get(root, "ids");
    if (!ids || !yyjson_is_arr(ids) || yyjson_arr_size(ids) == 0) {
        yyjson_doc_free(doc);
        return 0;
    }

    /* Build comma-separated ID list */
    char id_list[4096] = {0};
    size_t idx, max;
    yyjson_val *idv;
    yyjson_arr_foreach(ids, idx, max, idv) {
        unsigned long long id_val = yyjson_get_uint(idv);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%s%llu", id_list[0] ? "," : "", id_val);
        strncat(id_list, tmp, sizeof(id_list) - strlen(id_list) - 1);
        if (strlen(id_list) > 4000) break;
    }
    yyjson_doc_free(doc);
    if (!id_list[0]) return 0;

    /* Use /song/detail with comma-separated IDs */
    char sd_path[4608];
    snprintf(sd_path, sizeof(sd_path), "/song/detail?ids=%s", id_list);
    WriteBuf buf2 = {0};
    if (api_get(sd_path, &buf2) != 0) return -1;

    yyjson_doc *doc2 = yyjson_read(buf2.data, buf2.len, 0);
    free(buf2.data);
    if (!doc2) return -1;

    yyjson_val *songs = yyjson_obj_get(yyjson_doc_get_root(doc2), "songs");
    if (songs && yyjson_is_arr(songs)) {
        size_t n = yyjson_arr_size(songs);
        out->songs = (SongInfo*)calloc(n, sizeof(SongInfo));
        out->count = 0;
        size_t i, m;
        yyjson_val *s;
        yyjson_arr_foreach(songs, i, m, s) {
            SongInfo *si = &out->songs[out->count++];
            yyjson_val *v = yyjson_obj_get(s, "id");
            if (v) {
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "%llu", (unsigned long long)yyjson_get_uint(v));
                si->id = strdup(id_buf);
            }
            si->source = strdup("netease");
            v = yyjson_obj_get(s, "name");
            si->title = v ? strdup(yyjson_get_str(v)) : strdup("");
            yyjson_val *ar = yyjson_obj_get(s, "ar");
            if (ar && yyjson_arr_size(ar) > 0) {
                v = yyjson_obj_get(yyjson_arr_get(ar, 0), "name");
                si->artist = v ? strdup(yyjson_get_str(v)) : strdup("");
            } else si->artist = strdup("");
            v = yyjson_obj_get(s, "dt");
            si->duration_sec = v ? (int)(yyjson_get_int(v) / 1000) : 0;
        }
        out->total = out->count;
    }
    yyjson_doc_free(doc2);
    return out->count > 0 ? 0 : -1;
}

/* ── Init / Shutdown ────────────────────────────────── */
int netease_api_init(void) {
    if (g_curl) return 0;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
    if (!g_curl) return -1;
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(g_curl, CURLOPT_COOKIEFILE, "");

    /* Start API server if not already running */
    if (server_alive() != 0) {
        if (start_server() != 0) {
            LOG_WARN("API server unavailable; Netease features disabled");
        }
    }

    /* Restore persisted cookie, then refresh to get uid/nickname */
    cookie_load();
    if (g_cookie[0]) {
        LOG_INFO("Saved cookie loaded, refreshing login status...");
        netease_refresh_login();
        if (g_user_uid > 0) {
            LOG_INFO("Cookie persisted from previous session: %s (uid=%lu)",
                     g_account_name, g_user_uid);
        }
    }

    LOG_INFO("Netease API client ready");
    return 0;
}

void netease_api_shutdown(void) {
    /* Kill child API server */
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGTERM);
        waitpid(g_server_pid, NULL, 0);
        g_server_pid = 0;
        LOG_INFO("API server stopped");
    }
    if (g_curl) { curl_easy_cleanup(g_curl); g_curl = NULL; }
    curl_global_cleanup();
}

/* ── Override base URL (optional, for tests) ────────── */
void netease_api_set_base_url(const char *url) {
    if (url) snprintf(g_base_url, sizeof(g_base_url), "%s", url);
}

/* ── Search ─────────────────────────────────────────── */
int netease_search(const char *keyword, int limit, int offset,
                   NeteaseSearchResult *out) {
    if (!keyword || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* URL-encode the keyword for Chinese/UTF-8 support */
    char encoded[1024];
    {
        int ei = 0;
        for (const char *p = keyword; *p && ei < (int)sizeof(encoded) - 4; p++) {
            unsigned char c = (unsigned char)*p;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.') {
                encoded[ei++] = (char)c;
            } else if (c == ' ') {
                encoded[ei++] = '+';
            } else {
                int n = snprintf(encoded + ei, sizeof(encoded) - (size_t)ei,
                                 "%%%02X", c);
                ei += n;
            }
        }
        encoded[ei] = '\0';
    }
    char path[1024];
    snprintf(path, sizeof(path), "/search?keywords=%s&limit=%d&offset=%d&type=1",
             encoded, limit, offset);

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

    yyjson_doc *doc = parse_ok(&buf);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *result = yyjson_obj_get(root, "result");
    yyjson_val *songs = result ? yyjson_obj_get(result, "songs") : NULL;
    if (!songs) { yyjson_doc_free(doc); return 0; }

    size_t n = yyjson_arr_size(songs);
    out->songs = (NeteaseSongResult*)calloc(n, sizeof(NeteaseSongResult));
    out->count = (int)n;

    size_t idx, max;
    yyjson_val *song;
    yyjson_arr_foreach(songs, idx, max, song) {
        NeteaseSongResult *s = &out->songs[idx];
        yyjson_val *v;

        v = yyjson_obj_get(song, "id");
        if (v) {
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "%llu",
                     (unsigned long long)yyjson_get_uint(v));
            s->id = strdup(id_buf);
        }

        v = yyjson_obj_get(song, "name");
        s->title = v ? strdup(yyjson_get_str(v)) : strdup("");

        yyjson_val *artists = yyjson_obj_get(song, "artists");
        if (artists && yyjson_arr_size(artists) > 0) {
            v = yyjson_obj_get(yyjson_arr_get(artists, 0), "name");
            s->artist = v ? strdup(yyjson_get_str(v)) : strdup("");
        }

        yyjson_val *album = yyjson_obj_get(song, "album");
        if (album) {
            v = yyjson_obj_get(album, "name");
            s->album = v ? strdup(yyjson_get_str(v)) : strdup("");
        }

        v = yyjson_obj_get(song, "duration");
        s->duration_ms = v ? (int)yyjson_get_int(v) : 0;
    }

    yyjson_doc_free(doc);
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

/* ── Helper: current timestamp in milliseconds ─────── */

/* Inject saved cookies into curl engine with domain=localhost,
   so they're sent on all subsequent API calls regardless of original domain. */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

/* ── QR Login ────────────────────────────────────────── */
/* Step 1: Get unikey from Netease */
int netease_qr_get_key(char *out_unikey, size_t unikey_size,
                        char *qr_url, size_t url_size) {
    char path[256];
    snprintf(path, sizeof(path), "/login/qr/key?timestamp=%lld", now_ms());

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

    yyjson_doc *doc = parse_ok(&buf);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = yyjson_obj_get(root, "data");
    if (data) {
        yyjson_val *v = yyjson_obj_get(data, "unikey");
        if (v && out_unikey) snprintf(out_unikey, unikey_size, "%s", yyjson_get_str(v));
        v = yyjson_obj_get(data, "qrimg");
        if (v && qr_url) {
            /* qr_url gets the base64 qr image, for terminal display */
            snprintf(qr_url, url_size, "%s", yyjson_get_str(v));
        }
    }

    yyjson_doc_free(doc);
    return 0;
}

/* Step 1.5: Get full QR URL via /login/qr/create (adds chainId for web platform).
 * Returns the complete QR login URL. out_url must be >= 512 bytes. */
int netease_qr_create(const char *unikey, char *out_url, size_t url_size) {
    char path[512];
    snprintf(path, sizeof(path), "/login/qr/create?key=%s&platform=web&timestamp=%lld",
             unikey ? unikey : "", now_ms());

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = yyjson_obj_get(root, "data");
    yyjson_val *v = NULL;
    if (data) v = yyjson_obj_get(data, "qrurl");
    if (v && yyjson_get_str(v)) {
        snprintf(out_url, url_size, "%s", yyjson_get_str(v));
        yyjson_doc_free(doc);
        return 0;
    }

    yyjson_doc_free(doc);
    return -1;
}

/* Step 2: Poll login status.
 * Returns: 0 = logged in (803), 1 = waiting for scan (801),
 *          2 = expired (800), 3 = scanned, confirm on phone (802),
 *          <0 = error.
 * On success (0) reads cookies and saves them automatically. */
int netease_qr_poll(const char *unikey) {
    char path[512];
    snprintf(path, sizeof(path), "/login/qr/check?key=%s&timestamp=%lld",
             unikey ? unikey : "", now_ms());

    WriteBuf buf = {0};
    g_capture_cookies = true;
    int rc = api_get(path, &buf);
    g_capture_cookies = false;
    if (rc != 0) return -1;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *code = yyjson_obj_get(root, "code");
    int ret;
    if (code) {
        int c = (int)yyjson_get_int(code);
        switch (c) {
        case 800: ret = 2; break;   /* expired */
        case 801: ret = 1; break;   /* waiting for scan */
        case 802: ret = 3; break;   /* scanned, waiting for phone confirm */
        case 803: ret = 0; break;   /* authorized, login successful */
        default:  ret = 1; break;
        }
    } else {
        ret = -1;
    }

    /* On success (803): fetch profile first, then save cookie with uid/nickname. */
    if (ret == 0) {
        LOG_INFO("QR login success (803)");
        netease_refresh_login();
        LOG_INFO("After refresh: uid=%lu name=%s", g_user_uid, g_account_name);
        cookie_save();
        LOG_INFO("Cookie saved to %s (uid=%lu)", g_cookie_path, g_user_uid);
    }

    yyjson_doc_free(doc);
    return ret;
}

/* ── Refresh login status ──────────────────────────── */
/* After receiving the MUSIC_U cookie (from QR login), call /login/status
   to populate g_user_uid and g_account_name. */
void netease_refresh_login(void) {
    char path[256];
    snprintf(path, sizeof(path), "/login/status?timestamp=%lld", now_ms());

    LOG_INFO("Refreshing login (g_cookie=%.100s)", g_cookie);

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) {
        LOG_WARN("Refresh login api_get failed");
        return;
    }

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = yyjson_obj_get(root, "data");
    if (!data) { yyjson_doc_free(doc); return; }

    yyjson_val *profile = yyjson_obj_get(data, "profile");
    if (profile) {
        yyjson_val *uid_v = yyjson_obj_get(profile, "userId");
        if (uid_v) g_user_uid = (unsigned long)yyjson_get_uint(uid_v);

        yyjson_val *nick = yyjson_obj_get(profile, "nickname");
        if (nick) snprintf(g_account_name, sizeof(g_account_name), "%s", yyjson_get_str(nick));
    }

    if (g_account_name[0])
        LOG_INFO("Netease login refreshed: %s (uid=%lu)", g_account_name, g_user_uid);
    else
        LOG_WARN("Netease /login/status returned no profile (cookie may be invalid)");

    yyjson_doc_free(doc);
}

bool netease_is_logged_in(void) {
    return g_account_name[0] != 0;
}

const char* netease_account_name(void) {
    return g_account_name[0] ? g_account_name : NULL;
}

/* ── Song detail ────────────────────────────────────── */
int netease_get_song_detail(const char *song_id,
                            char **title, char **artist, char **album,
                            int *duration_ms) {
    char path[512];
    snprintf(path, sizeof(path), "/song/detail?ids=%%5B%s%%5D", song_id);

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

    yyjson_doc *doc = parse_ok(&buf);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *songs = yyjson_obj_get(root, "songs");
    if (!songs || yyjson_arr_size(songs) == 0) {
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_val *song = yyjson_arr_get(songs, 0);
    yyjson_val *v;

    if (title) {
        v = yyjson_obj_get(song, "name");
        *title = v ? strdup(yyjson_get_str(v)) : strdup("");
    }
    if (artist) {
        yyjson_val *artists = yyjson_obj_get(song, "artists");
        if (artists && yyjson_arr_size(artists) > 0) {
            v = yyjson_obj_get(yyjson_arr_get(artists, 0), "name");
            *artist = v ? strdup(yyjson_get_str(v)) : strdup("");
        } else {
            *artist = strdup("");
        }
    }
    if (album) {
        yyjson_val *al = yyjson_obj_get(song, "album");
        if (al) {
            v = yyjson_obj_get(al, "name");
            *album = v ? strdup(yyjson_get_str(v)) : strdup("");
        } else {
            *album = strdup("");
        }
    }
    if (duration_ms) {
        v = yyjson_obj_get(song, "duration");
        *duration_ms = v ? (int)yyjson_get_int(v) : 0;
    }

    yyjson_doc_free(doc);
    return 0;
}

/* ── Play URL ────────────────────────────────────────── */
int netease_get_play_url(const char *song_id, int quality,
                         char *out_url, size_t url_size) {
    const char *br = "128000";
    if (quality == 1) br = "192000";
    else if (quality >= 2) br = "320000";

    char path[512];
    snprintf(path, sizeof(path), "/song/url/v1?id=%s&level=%s", song_id,
             quality >= 2 ? "lossless" : (quality == 1 ? "exhigh" : "standard"));

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) { *out_url = '\0'; return -1; }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *code_v = yyjson_obj_get(root, "code");
    int code = code_v ? (int)yyjson_get_int(code_v) : -1;

    if (code != 200) {
        /* fallback to old endpoint */
        yyjson_doc_free(doc);
        snprintf(path, sizeof(path), "/song/url?id=%s&br=%s", song_id, br);
        WriteBuf buf2 = {0};
        if (api_get(path, &buf2) != 0) return -1;
        doc = yyjson_read(buf2.data, buf2.len, 0);
        free(buf2.data);
        if (!doc) { *out_url = '\0'; return -1; }
        root = yyjson_doc_get_root(doc);
    }

    yyjson_val *data = yyjson_obj_get(root, "data");
    if (data && yyjson_arr_size(data) > 0) {
        yyjson_val *item = yyjson_arr_get(data, 0);
        yyjson_val *url_v = yyjson_obj_get(item, "url");
        if (url_v && yyjson_get_str(url_v)) {
            snprintf(out_url, url_size, "%s", yyjson_get_str(url_v));
            yyjson_doc_free(doc);
            return 0;
        }
    }

    *out_url = '\0';
    yyjson_doc_free(doc);
    return -1;
}

/* ── Lyrics ──────────────────────────────────────────── */
int netease_get_lyric(const char *song_id, char *buf, size_t buf_size) {
    char path[512];
    snprintf(path, sizeof(path), "/lyric?id=%s", song_id);

    WriteBuf wb = {0};
    if (api_get(path, &wb) != 0) return -1;

    yyjson_doc *doc = parse_ok(&wb);
    free(wb.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *lrc = yyjson_obj_get(root, "lrc");
    yyjson_val *lyric = lrc ? yyjson_obj_get(lrc, "lyric") : NULL;
    if (lyric && yyjson_get_str(lyric))
        snprintf(buf, buf_size, "%s", yyjson_get_str(lyric));
    else if (buf_size > 0)
        buf[0] = '\0';

    yyjson_doc_free(doc);
    return 0;
}

/* ── Cover URL ──────────────────────────────────────── */
int netease_get_cover_url(const char *song_id, char *buf, size_t buf_size) {
    char *album = NULL;
    netease_get_song_detail(song_id, NULL, NULL, &album, NULL);
    if (buf_size > 0) buf[0] = '\0';
    free(album);
    return 0;
}

/* ── Load menu item songs ────────────────────────────── */
int netease_load_menu(int type, int limit, SearchResult *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    char path_buf[256];
    const char *tmpl = NULL;
    switch (type) {
    case 0:  tmpl = "/personalized/newsong?limit=%d"; break;
    case 1:  tmpl = "/personalized?limit=%d"; break;
    default: return -1;
    }
    snprintf(path_buf, sizeof(path_buf), tmpl, limit > 0 ? limit : 30);

    WriteBuf buf = {0};
    if (api_get(path_buf, &buf) != 0) return -1;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *result = yyjson_obj_get(root, "result");
    if (!result) { yyjson_doc_free(doc); return 0; }

    yyjson_val *songs = yyjson_obj_get(result, "songs");
    if (!songs) songs = result; /* personalized returns result array directly */
    if (!yyjson_is_arr(songs)) { yyjson_doc_free(doc); return 0; }

    size_t n = yyjson_arr_size(songs);
    size_t count = (limit > 0 && n > (size_t)limit) ? (size_t)limit : n;
    out->songs = (SongInfo*)calloc(count, sizeof(SongInfo));
    out->count = (int)count;
    out->total = (int)count;

    size_t idx, max;
    yyjson_val *item;
    size_t oi = 0;
    yyjson_arr_foreach(songs, idx, max, item) {
        if (oi >= count) break;
        SongInfo *si = &out->songs[oi++];
        yyjson_val *v;

        /* song id */
        v = yyjson_obj_get(item, "id");
        if (v) {
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "%llu",
                     (unsigned long long)yyjson_get_uint(v));
            si->id = strdup(id_buf);
        }
        si->source = strdup("netease");

        v = yyjson_obj_get(item, "name");
        si->title = v ? strdup(yyjson_get_str(v)) : strdup("");

        yyjson_val *artists = yyjson_obj_get(item, "artists");
        if (artists && yyjson_arr_size(artists) > 0) {
            v = yyjson_obj_get(yyjson_arr_get(artists, 0), "name");
            si->artist = v ? strdup(yyjson_get_str(v)) : strdup("");
        } else {
            si->artist = strdup("");
        }

        v = yyjson_obj_get(item, "duration");
        si->duration_sec = v ? (int)(yyjson_get_int(v) / 1000) : 0;
    }

    yyjson_doc_free(doc);
    return 0;
}

/* ── Playlists (for menu items) ─────────────────────── */
int netease_get_personalized(char *out_json, size_t json_size) {
    WriteBuf buf = {0};
    if (api_get("/personalized?limit=30", &buf) != 0) return -1;
    snprintf(out_json, json_size, "%s", buf.data ? buf.data : "[]");
    free(buf.data);
    return 0;
}
