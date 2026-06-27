#include "netease_api.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <yyjson.h>

/* ── Internal state ─────────────────────────────────── */
static CURL *g_curl = NULL;
static char  g_cookie[4096] = {0};
static char  g_account_name[128] = {0};
static bool  g_inited = false;

/* ── CURL write callback ────────────────────────────── */
typedef struct { char *data; size_t len; size_t cap; } WriteBuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    WriteBuf *buf = (WriteBuf*)userdata;
    if (buf->len + total + 1 > buf->cap) {
        buf->cap = buf->cap ? buf->cap * 2 : 8192;
        buf->data = (char*)realloc(buf->data, buf->cap);
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static CURL* api_curl(void) {
    if (!g_curl) {
        g_curl = curl_easy_init();
        if (!g_curl) return NULL;
        curl_easy_setopt(g_curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
        curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(g_curl, CURLOPT_FOLLOWLOCATION, 1L);
    }
    return g_curl;
}

static int api_request(const char *url, const char *post_data,
                        WriteBuf *out) {
    CURL *curl = api_curl();
    if (!curl) return -1;

    memset(out, 0, sizeof(*out));
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Referer: https://music.163.com");
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    if (g_cookie[0])
        curl_easy_setopt(curl, CURLOPT_COOKIE, g_cookie);

    if (post_data) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_data));
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        LOG_WARN("HTTP request failed: %s", curl_easy_strerror(res));
        return -1;
    }

    /* Read Set-Cookie headers */
    if (g_cookie[0] == 0) {
        struct curl_slist *cookies = NULL;
        curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies);
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
            if (buf[0]) {
                snprintf(g_cookie, sizeof(g_cookie), "%s", buf);
            }
        }
    }

    return 0;
}

/* ── Init / shutdown ────────────────────────────────── */
int netease_api_init(void) {
    if (g_inited) return 0;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_inited = true;
    LOG_INFO("Netease API initialized");
    return 0;
}

void netease_api_shutdown(void) {
    if (g_curl) { curl_easy_cleanup(g_curl); g_curl = NULL; }
    if (g_inited) { curl_global_cleanup(); g_inited = false; }
    LOG_INFO("Netease API shutdown");
}

/* ── Search ─────────────────────────────────────────── */
int netease_search(const char *keyword, int limit, int offset,
                   NeteaseSearchResult *out) {
    if (!keyword || !out) return -1;
    memset(out, 0, sizeof(*out));

    char post[1024];
    snprintf(post, sizeof(post),
        "s=%s&type=1&limit=%d&offset=%d",
        keyword, limit > 0 ? limit : 20, offset > 0 ? offset : 0);

    WriteBuf buf = {0};
    int rc = api_request("https://music.163.com/api/search/get", post, &buf);
    if (rc != 0) return rc;

    /* Parse JSON */
    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *result = yyjson_obj_get(root, "result");
    if (!result) { yyjson_doc_free(doc); return -1; }

    yyjson_val *songs = yyjson_obj_get(result, "songs");
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
            snprintf(id_buf, sizeof(id_buf), "%llu", (unsigned long long)yyjson_get_int(v));
            s->id = strdup(id_buf);
        } else {
            s->id = strdup("");
        }

        v = yyjson_obj_get(song, "name");
        s->title = v ? strdup(yyjson_get_str(v)) : strdup("");

        /* Get first artist */
        yyjson_val *artists = yyjson_obj_get(song, "artists");
        if (artists && yyjson_arr_size(artists) > 0) {
            yyjson_val *a = yyjson_arr_get_first(artists);
            v = yyjson_obj_get(a, "name");
            s->artist = v ? strdup(yyjson_get_str(v)) : strdup("");
        } else {
            s->artist = strdup("");
        }

        v = yyjson_obj_get(song, "album");
        if (v) {
            yyjson_val *an = yyjson_obj_get(v, "name");
            s->album = an ? strdup(yyjson_get_str(an)) : strdup("");
        } else {
            s->album = strdup("");
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
    int rc = api_request(
        "https://music.163.com/api/login/qrcode/unikey?type=1", NULL, &buf);
    if (rc != 0) return rc;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = yyjson_obj_get(root, "data");
    if (!data) { yyjson_doc_free(doc); return -1; }

    yyjson_val *uk = yyjson_obj_get(data, "unikey");
    if (uk) snprintf(out_unikey, unikey_size, "%s", yyjson_get_str(uk));

    yyjson_val *qr = yyjson_obj_get(data, "qrurl");
    if (qr) snprintf(qr_url, url_size, "%s", yyjson_get_str(qr));

    yyjson_doc_free(doc);
    return 0;
}

int netease_qr_poll(const char *unikey) {
    char url[512];
    snprintf(url, sizeof(url),
        "https://music.163.com/api/login/qrcode/client/login?key=%s&type=1",
        unikey ? unikey : "");

    WriteBuf buf = {0};
    int rc = api_request(url, NULL, &buf);
    if (rc != 0) return -1;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *code = yyjson_obj_get(root, "code");

    /* Extract account name on success */
    yyjson_val *cookie_str = yyjson_obj_get(root, "cookie");
    if (cookie_str) {
        /* Store full cookie from response */
        const char *ck = yyjson_get_str(cookie_str);
        if (ck) snprintf(g_cookie, sizeof(g_cookie), "%s", ck);
    }

    int ret;
    if (code) {
        int c = (int)yyjson_get_int(code);
        switch (c) {
        case 800: ret = 2; break;   /* expired */
        case 802: ret = 0; break;   /* logged in */
        case 801: ret = 1; break;   /* waiting */
        default:  ret = 1; break;
        }
    } else {
        ret = -1;
    }

    if (ret == 0) {
        /* Try to extract nickname */
        yyjson_val *profile = yyjson_obj_get(root, "profile");
        if (profile) {
            yyjson_val *nn = yyjson_obj_get(profile, "nickname");
            if (nn) {
                snprintf(g_account_name, sizeof(g_account_name), "%s", yyjson_get_str(nn));
                LOG_INFO("Netease login: %s", g_account_name);
            }
        }
    }

    yyjson_doc_free(doc);
    return ret;
}

bool netease_is_logged_in(void) {
    return g_cookie[0] != 0;
}

const char* netease_account_name(void) {
    return g_account_name[0] ? g_account_name : NULL;
}

/* ── Song detail ────────────────────────────────────── */
int netease_get_song_detail(const char *song_id,
                            char **title, char **artist, char **album,
                            int *duration_ms) {
    char url[512];
    snprintf(url, sizeof(url),
        "https://music.163.com/api/song/detail?id=%s&ids=%%5B%s%%5D",
        song_id, song_id);

    WriteBuf buf = {0};
    int rc = api_request(url, NULL, &buf);
    if (rc != 0) return rc;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *songs = yyjson_obj_get(root, "songs");
    if (!songs || yyjson_arr_size(songs) == 0) {
        yyjson_doc_free(doc);
        return -1;
    }

    yyjson_val *song = yyjson_arr_get_first(songs);
    yyjson_val *v;

    v = yyjson_obj_get(song, "name");
    if (title) *title = v ? strdup(yyjson_get_str(v)) : strdup("");

    yyjson_val *artists = yyjson_obj_get(song, "artists");
    if (artists && yyjson_arr_size(artists) > 0 && artist) {
        v = yyjson_obj_get(yyjson_arr_get_first(artists), "name");
        *artist = v ? strdup(yyjson_get_str(v)) : strdup("");
    } else if (artist) {
        *artist = strdup("");
    }

    v = yyjson_obj_get(song, "album");
    if (v && album) {
        yyjson_val *an = yyjson_obj_get(v, "name");
        *album = an ? strdup(yyjson_get_str(an)) : strdup("");
    } else if (album) {
        *album = strdup("");
    }

    v = yyjson_obj_get(song, "duration");
    if (duration_ms) *duration_ms = v ? (int)yyjson_get_int(v) : 0;

    yyjson_doc_free(doc);
    return 0;
}

/* ── Play URL ──────────────────────────────────────────
 * Note: Netease's play URL API requires encrypted parameters.
 * This is a simplified version that may not work for all songs.
 * ───────────────────────────────────────────────────── */
int netease_get_play_url(const char *song_id, int quality,
                         char *out_url, size_t url_size) {
    if (!song_id || !out_url) return -1;

    char post[512];
    const char *br = "128000";
    if (quality == 1) br = "192000";
    else if (quality >= 2) br = "320000";

    snprintf(post, sizeof(post),
        "ids=%%5B%s%%5D&br=%s",
        song_id, br);

    WriteBuf buf = {0};
    int rc = api_request(
        "https://music.163.com/api/song/enhance/player/url", post, &buf);
    if (rc != 0) return rc;

    yyjson_doc *doc = yyjson_read(buf.data, buf.len, 0);
    free(buf.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = yyjson_obj_get(root, "data");
    if (!data || yyjson_arr_size(data) == 0) {
        yyjson_doc_free(doc);
        *out_url = '\0';
        return -1;
    }

    yyjson_val *item = yyjson_arr_get_first(data);
    yyjson_val *url_v = yyjson_obj_get(item, "url");
    if (url_v && yyjson_get_str(url_v)) {
        snprintf(out_url, url_size, "%s", yyjson_get_str(url_v));
        yyjson_doc_free(doc);
        return 0;
    }

    /* url may be null if song requires higher auth (paid, etc.) */
    *out_url = '\0';
    yyjson_doc_free(doc);
    return -1;
}

/* ── Lyrics ──────────────────────────────────────────── */
int netease_get_lyric(const char *song_id, char *buf, size_t buf_size) {
    char url[512];
    snprintf(url, sizeof(url),
        "https://music.163.com/api/song/lyric?id=%s&lv=-1&kv=-1&tv=-1",
        song_id);

    WriteBuf wb = {0};
    int rc = api_request(url, NULL, &wb);
    if (rc != 0) return rc;

    yyjson_doc *doc = yyjson_read(wb.data, wb.len, 0);
    free(wb.data);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *lrc = yyjson_obj_get(root, "lrc");
    yyjson_val *lyric = lrc ? yyjson_obj_get(lrc, "lyric") : NULL;

    if (lyric && yyjson_get_str(lyric)) {
        snprintf(buf, buf_size, "%s", yyjson_get_str(lyric));
    } else {
        if (buf_size > 0) buf[0] = '\0';
    }

    yyjson_doc_free(doc);
    return 0;
}

/* ── Cover URL ──────────────────────────────────────── */
int netease_get_cover_url(const char *song_id, char *buf, size_t buf_size) {
    char *album = NULL;
    int rc = netease_get_song_detail(song_id, NULL, NULL, &album, NULL);
    if (rc != 0) return rc;

    /* The song detail returns album info but the cover URL is not directly
       in the same call. For now, return the song detail approach. */
    /* Simplify: just return empty */
    if (buf_size > 0) buf[0] = '\0';
    free(album);
    return 0;
}
