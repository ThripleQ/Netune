#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stddef.h>
#include "core/music_source.h"

/* netease_ext.h — 网易云特化接口。
 *
 * 独立于通用插件接口 MusicSource（core/music_source.h）：通用接口描述
 * 所有音乐源共有的能力；这里描述网易云独有能力（登录/歌单/下载/已购/
 * 音质/VIP）。应用层（app.cpp、download_queue 等）只依赖本接口头 +
 * netease_ext()，不直接依赖 netease_api.h 的实现细节。
 *
 * 类型定义（NSSong/NSSearchResult/SongDetail/NQ_*）随接口放在本头，
 * netease_api.h 改为引用本头，避免应用层因类型被迫 include 实现头。
 */

/* ── 类型（原 netease_api.h，随接口上移）────────────── */
typedef struct { char *id, *title, *artist, *album, *cover_url; int dur_ms; int fee; } NSSong;
typedef struct { NSSong *songs; int count; } NSSearchResult;

typedef struct {
    char *title;
    char *artist;      /* joined artists */
    char *album;
    int   duration_sec;
    int   fee;         /* 0 free, 1/8 vip, 4 paid */
    char *publish;     /* YYYY-MM-DD (local) */
    int   pop;         /* hotness 0..100, -1 unknown */
    char *cover_url;
} SongDetail;

/* per-tier source bitmask (song-music-quality), high→low */
#define NQ_JYMASTER  (1u << 0)
#define NQ_SKY       (1u << 1)
#define NQ_JYEFFECT  (1u << 2)
#define NQ_HIRES     (1u << 3)
#define NQ_LOSSLESS  (1u << 4)
#define NQ_EXHIGH    (1u << 5)
#define NQ_HIGHER    (1u << 6)
#define NQ_STANDARD  (1u << 7)
#define NQ_LEVELS    8

/* download progress callback (invoked from the libcurl worker thread) */
typedef void (*netease_download_progress)(void *ud, long long done,
                                          long long total);

/* ── 特化接口函数表 ────────────────────────────────── */
typedef struct NeteaseExt {
    /* account */
    const char* (*account_name)(void);
    bool (*is_logged_in)(void);
    int  (*login_refresh)(void);
    int  (*logout)(void);

    /* QR login */
    int  (*qr_key)(char *unikey, size_t uk_sz, char *url, size_t url_sz);
    char* (*qr_render)(const char *url);
    char* (*qr_image)(const char *url);
    int  (*qr_poll)(const char *unikey);

    /* search */
    int  (*search)(const char *kw, int limit, int offset, NSSearchResult *out);
    void (*search_free)(NSSearchResult *r);
    int  (*search_playlists)(const char *kw, SongInfo **out, int *count);
    int  (*check_music)(const char *song_id, bool *playable);

    /* playlists */
    int  (*playlists)(bool favorited, SongInfo **out, int *count);
    int  (*playlist_songs)(const char *id, SongInfo **out, int *count);
    int  (*liked_songs)(SongInfo **out, int *count);
    int  (*menu_songs)(int type, int limit, SongInfo **out, int *count);
    int  (*daily_playlists)(SongInfo **out, int *count);
    int  (*recent_songs)(SongInfo **out, int *count);
    int  (*toplist)(SongInfo **out, int *count);
    int  (*like_song)(const char *song_id, bool like);
    int  (*liked_check)(const char *song_id, bool *liked);
    int  (*subscribe_playlist)(const char *pl_id, bool sub);
    int  (*track_add)(const char *pl_id, const char *song_id);
    int  (*track_remove)(const char *pl_id, const char *song_id);
    int  (*playlist_create)(const char *name, char *new_id, size_t id_sz);
    int  (*playlist_rename)(const char *pl_id, const char *name);
    int  (*playlist_delete)(const char *pl_id);

    /* song detail */
    int  (*song_detail)(const char *song_id, SongDetail *out);
    void (*song_detail_free)(SongDetail *d);

    /* quality / vip */
    int  (*song_music_quality)(const char *song_id, unsigned *mask_out,
                               int *br_out);
    int  (*vip_level)(void);
    /* play endpoint URL at an explicit quality — the authority for "can
       this tier stream": 0 = a real url is served (code 200), -1 = denied.
       Purely result-driven. */
    int  (*song_url)(const char *song_id, const char *level,
                     char *url, size_t url_sz);

    /* ownership / purchases */
    int  (*song_owned)(const char *song_id, const char *level);
    int  (*purchased_refresh)(void);
    int  (*is_purchased)(const char *song_id);
    int  (*purchased_songs)(SongInfo **out, int *count);
    int  (*purchased_albums)(SongInfo **out, int *count);
    int  (*album_songs)(const char *album_id, SongInfo **out, int *count);

    /* download */
    char* (*download_song)(const char *song_id, const char *level,
                           const char *title, const char *artist,
                           char *used_level, size_t used_sz,
                           netease_download_progress prog, void *ud);
    int  (*check_quality)(const char *song_id, const char *level, int *granted);
    /* official download endpoint URL probe — the authority for "can I
       download this tier": 0 = a real url is served (code 200), -1 = denied
       (code -120 / no url). Purely result-driven entitlement. */
    int  (*download_url)(const char *song_id, const char *level,
                         char *url, size_t url_sz);

    /* lyric */
    int  (*lyric)(const char *song_id, char **buf);
} NeteaseExt;

/* 获取网易云特化接口（由 netease_source 注册，进程生命周期内有效） */
const NeteaseExt *netease_ext(void);

#ifdef __cplusplus
}
#endif
