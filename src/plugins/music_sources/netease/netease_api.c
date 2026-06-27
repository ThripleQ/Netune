#include "netease_api.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <curl/curl.h>
#include <yyjson.h>

/* ── Configuration ──────────────────────────────────── */
static char g_base_url[256] = "http://localhost:10000";
static pid_t g_server_pid = 0;

/* Path to the Node.js API server */
#define SERVER_DIR  "/home/liu/Projects/netease-api-local/node_modules/NeteaseCloudMusicApi"
#define SERVER_PORT "10000"
static char g_cookie[4096] = {0};
static char g_account_name[128] = {0};
static CURL *g_curl = NULL;

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
    if (g_cookie[0])
        curl_easy_setopt(g_curl, CURLOPT_COOKIE, g_cookie);

    /* Suppress curl output */
    curl_easy_setopt(g_curl, CURLOPT_VERBOSE, 0L);

    CURLcode res = curl_easy_perform(g_curl);
    if (res != CURLE_OK) {
        LOG_WARN("API request failed: %s", curl_easy_strerror(res));
        return -1;
    }

    /* Save cookies from response */
    struct curl_slist *cookies = NULL;
    curl_easy_getinfo(g_curl, CURLINFO_COOKIELIST, &cookies);
    if (cookies) {
        char buf[4096] = {0};
        struct curl_slist *nc = cookies;
        while (nc) {
            if (nc->data && nc->data[0] != '#') {
                if (buf[0]) strncat(buf, "; ", sizeof(buf) - strlen(buf) - 1);
                strncat(buf, nc->data, sizeof(buf) - strlen(buf) - 1);
            }
            nc = nc->next;
        }
        curl_slist_free_all(cookies);
        if (buf[0]) snprintf(g_cookie, sizeof(g_cookie), "%s", buf);
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
static int start_server(void) {
    if (g_server_pid > 0) return 0; /* already started */

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

    char path[1024];
    snprintf(path, sizeof(path), "/search?keywords=%s&limit=%d&offset=%d&type=1",
             keyword, limit, offset);

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
                     (unsigned long long)yyjson_get_int(v));
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

/* ── QR Login ────────────────────────────────────────── */
int netease_qr_get_key(char *out_unikey, size_t unikey_size,
                        char *qr_url, size_t url_size) {
    WriteBuf buf = {0};
    if (api_get("/login/qr/key?timestamp=1", &buf) != 0) return -1;

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

int netease_qr_poll(const char *unikey) {
    char path[512];
    snprintf(path, sizeof(path), "/login/qr/check?key=%s&timestamp=1",
             unikey ? unikey : "");

    WriteBuf buf = {0};
    if (api_get(path, &buf) != 0) return -1;

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
        case 802: ret = 0; break;   /* confirmed (logged in) */
        case 801: ret = 1; break;   /* waiting for scan */
        default:  ret = 1; break;
        }
    } else {
        ret = -1;
    }

    /* On success, extract nickname */
    if (ret == 0) {
        yyjson_val *nick = yyjson_obj_get(root, "nickname");
        if (nick) {
            snprintf(g_account_name, sizeof(g_account_name), "%s",
                     yyjson_get_str(nick));
            LOG_INFO("Netease login: %s", g_account_name);
        }
        /* API server stores cookies internally; we also save */
    }

    yyjson_doc_free(doc);
    return ret;
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

/* ── Playlists (for menu items) ─────────────────────── */
int netease_get_personalized(char *out_json, size_t json_size) {
    WriteBuf buf = {0};
    if (api_get("/personalized?limit=30", &buf) != 0) return -1;
    snprintf(out_json, json_size, "%s", buf.data ? buf.data : "[]");
    free(buf.data);
    return 0;
}
