#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* ── Song metadata ─────────────────────────────────── */
typedef struct {
    char *id;           /* unique source-specific id   */
    char *source;       /* "local", "netease", "qq"    */
    char *title;
    char *artist;
    char *album;
    int   duration_sec;
    char *cover_url;
    char *aux_label;    /* e.g. bitrate / format hint  */
} SongInfo;

/* ── Search result ─────────────────────────────────── */
typedef struct {
    SongInfo *songs;
    int       count;
    int       total;    /* total results available      */
} SearchResult;

/* ── Music source plugin interface ──────────────────── */
typedef struct MusicSource {
    const char *name;
    int         priority;    /* lower = higher priority  */

    int  (*init)(void);
    void (*shutdown)(void);

    /* search: 0=success, <0=error */
    int  (*search)(const char *keyword, int page, int page_size,
                   SearchResult *out);

    /* get song detail: 0=success */
    int  (*get_song_detail)(const char *song_id, SongInfo *out);

    /* get play URL (for streaming sources) */
    int  (*get_play_url)(const char *song_id, int quality,
                         char *url_buf, size_t url_size);

    /* get lyrics: 0=success, text allocated by caller */
    int  (*get_lyric)(const char *song_id, char *buf, size_t buf_size);

    /* get cover image URL */
    int  (*get_cover_url)(const char *song_id, char *buf, size_t buf_size);

    /* health check */
    bool (*is_available)(void);
} MusicSource;

/* ── Song info helpers ──────────────────────────────── */
void song_info_free(SongInfo *info);
void song_info_copy(SongInfo *dst, const SongInfo *src);
void search_result_free(SearchResult *result);

#ifdef __cplusplus
}
#endif
