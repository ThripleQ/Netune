#include "netease_source.h"
#include "netease_api.h"
#include "core/music_source_manager.h"
#include "infra/log.h"
#include <string.h>
#include <stdlib.h>

/* ── MusicSource implementation ──────────────────────── */
static int ns_init(void) {
    if (netease_api_init() != 0) {
        LOG_WARN("Netease source unavailable");
        return -1;
    }
    LOG_INFO("Netease source initialized");
    return 0;
}

static void ns_shutdown(void) {
    netease_api_shutdown();
    LOG_INFO("Netease source shutdown");
}

static int ns_search(const char *keyword, int page, int page_size,
                     SearchResult *out) {
    if (!keyword || !out) return -1;
    memset(out, 0, sizeof(*out));

    int limit = page_size > 0 ? page_size : 20;
    int offset = page > 0 ? page * limit : 0;

    NeteaseSearchResult nr;
    if (netease_search(keyword, limit, offset, &nr) != 0)
        return -1;

    if (nr.count <= 0) {
        netease_search_result_free(&nr);
        return 0;
    }

    out->songs = (SongInfo*)calloc((size_t)nr.count, sizeof(SongInfo));
    out->count = nr.count;
    out->total = nr.count;

    for (int i = 0; i < nr.count; i++) {
        SongInfo *si = &out->songs[i];
        si->id       = strdup(nr.songs[i].id);
        si->source   = strdup("netease");
        si->title    = strdup(nr.songs[i].title ? nr.songs[i].title : "");
        si->artist   = strdup(nr.songs[i].artist ? nr.songs[i].artist : "");
        si->album    = strdup(nr.songs[i].album ? nr.songs[i].album : "");
        si->duration_sec = nr.songs[i].duration_ms / 1000;
        si->cover_url = strdup("");
        si->aux_label = strdup("");
    }

    netease_search_result_free(&nr);
    return 0;
}

static int ns_get_song_detail(const char *song_id, SongInfo *out) {
    if (!song_id || !out) return -1;
    memset(out, 0, sizeof(*out));

    char *title = NULL, *artist = NULL, *album = NULL;
    int duration_ms = 0;
    if (netease_get_song_detail(song_id, &title, &artist, &album,
                                 &duration_ms) != 0)
        return -1;

    out->id       = strdup(song_id);
    out->source   = strdup("netease");
    out->title    = title;
    out->artist   = artist;
    out->album    = album;
    out->duration_sec = duration_ms / 1000;
    out->cover_url = strdup("");
    out->aux_label = strdup("");
    return 0;
}

static int ns_get_play_url(const char *song_id, int quality,
                           char *url, size_t url_size) {
    return netease_get_play_url(song_id, quality, url, url_size);
}

/* Stub: lyrics not yet implemented via netease-cli */
static int ns_get_lyric(const char *song_id, char *buf, size_t buf_size) {
    (void)song_id;
    if (buf_size > 0) buf[0] = '\0';
    return -1;
}

/* Stub: cover not yet implemented */
static int ns_get_cover_url(const char *song_id, char *buf, size_t buf_size) {
    (void)song_id;
    if (buf_size > 0) buf[0] = '\0';
    return -1;
}

static bool ns_is_available(void) {
    return true;
}

static MusicSource g_netease_source = {
    .name            = "netease",
    .priority        = 20,
    .init            = ns_init,
    .shutdown        = ns_shutdown,
    .search          = ns_search,
    .get_song_detail = ns_get_song_detail,
    .get_play_url    = ns_get_play_url,
    .get_lyric       = ns_get_lyric,
    .get_cover_url   = ns_get_cover_url,
    .is_available    = ns_is_available,
};

void netease_source_register(void) {
    music_source_register(&g_netease_source);
}

MusicSource* netease_source_create(void) {
    return &g_netease_source;
}
