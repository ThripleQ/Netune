#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stddef.h>
#include "core/music_source.h"

/* ── Netease API client (netease-cli backend) ──────── */

int  netease_init(void);
void netease_shutdown(void);
const char* netease_account_name(void);

/* ── Search ────────────────────────────────────────── */
/* ⚠️ 加字段后同步修改: netease_api.c(fill/search), netease_source.c(ns_search) */
/*    app.cpp 手动构造 NSSong 的地方 */
typedef struct { char *id, *title, *artist, *album, *cover_url; int dur_ms; int fee; } NSSong;
typedef struct { NSSong *songs; int count; } NSSearchResult;

int  netease_search(const char *kw, int limit, int offset, NSSearchResult *out);
void netease_search_free(NSSearchResult *r);
int  netease_search_playlists(const char *kw, SongInfo **out, int *count); /* playlist results, is_playlist=1 */
int  netease_check_music(const char *song_id, bool *playable); /* 0 = ok */
int  netease_recent_songs(SongInfo **out, int *count);        /* recently played (needs login) */
int  netease_daily_playlists(SongInfo **out, int *count);     /* daily recommend playlists */

/* ── Login ─────────────────────────────────────────── */
int  netease_qr_key(char *unikey, size_t uk_sz, char *url, size_t url_sz);
char* netease_qr_render(const char *url);
char* netease_qr_image(const char *url);
int  netease_qr_poll(const char *unikey);
bool netease_is_logged_in(void);

/* ── Playlists ─────────────────────────────────────── */
int  netease_playlists(bool favorited, SongInfo **out, int *count);
int  netease_playlist_songs(const char *id,        /* songs in a playlist */
                            SongInfo **out, int *count);
int  netease_liked_songs(SongInfo **out, int *count);   /* liked songs */
int  netease_menu_songs(int type, int limit,            /* daily etc */
                        SongInfo **out, int *count);
int  netease_login_refresh(void);                       /* 0 = ok */
int  netease_logout(void);                              /* 0 = ok */
int  netease_like_song(const char *song_id, bool like); /* 0 = ok */
int  netease_liked_check(const char *song_id, bool *liked); /* 0 = ok, liked set */
int  netease_subscribe_playlist(const char *pl_id, bool sub); /* 0 = ok */
int  netease_toplist(SongInfo **out, int *count);       /* chart list */
/* ── Song detail ──────────────────────────────────── */
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
void song_detail_free(SongDetail *d);
int  netease_song_detail(const char *song_id, SongDetail *out); /* 0 = ok */
int  netease_track_add(const char *pl_id, const char *song_id);   /* 0 = ok */
int  netease_track_remove(const char *pl_id, const char *song_id);/* 0 = ok */
int  netease_playlist_create(const char *name, char *new_id, size_t id_sz); /* 0 = ok */
int  netease_playlist_rename(const char *pl_id, const char *name);   /* 0 = ok */
int  netease_playlist_delete(const char *pl_id);                    /* 0 = ok */

/* ── Play URL + Download ──────────────────────────── */
/* Get streaming URL.                                      */
int  netease_play_url(const char *song_id, char *url, size_t url_sz);

/* Get a play/download URL at an explicit quality level
   (standard/higher/exhigh/lossless/hires). 0 = ok, url filled. */
int  netease_song_url(const char *song_id, const char *level,
                      char *url, size_t url_sz);

/* Download a song (resolved at `level`, falling back down the quality
   ladder when unavailable) into the app download dir
   (netune_data_root()/downloads). Returns a malloc'd full path to the
   saved file, or NULL on failure. Caller frees. `used_level` (optional)
   receives the quality actually used. */
char* netease_download_song(const char *song_id, const char *level,
                            const char *title, char *used_level,
                            size_t used_sz);

/* Single-level entitlement probe (check-quality): *granted=1 only when the
   play endpoint would serve `level` (standard/higher/exhigh/lossless/hires)
   for this song. 0 = ok, *granted set; -1 = probe failed. */
int  netease_check_quality(const char *song_id, const char *level,
                           int *granted);

/* Authoritative per-tier source probe (song-music-quality): *mask_out
   receives a bitmask (NQ_JYMASTER|NQ_SKY|NQ_JYEFFECT|NQ_HIRES|NQ_LOSSLESS|
   NQ_EXHIGH|NQ_HIGHER|NQ_STANDARD) of which levels the track actually has
   a file for, high→low. br_out (optional, NQ_LEVELS ints, same order)
   receives each tier's bitrate, 0 = no source. 0 = ok, -1 = failed. */
#define NQ_JYMASTER  (1u << 0)
#define NQ_SKY       (1u << 1)
#define NQ_JYEFFECT  (1u << 2)
#define NQ_HIRES     (1u << 3)
#define NQ_LOSSLESS  (1u << 4)
#define NQ_EXHIGH    (1u << 5)
#define NQ_HIGHER    (1u << 6)
#define NQ_STANDARD  (1u << 7)
#define NQ_LEVELS    8
int  netease_song_music_quality(const char *song_id, unsigned *mask_out,
                                int *br_out);

/* Official download endpoint (song-download-url). Unlike the play URL this
   channel can serve up to Hi-Res even for free tracks, but VIP-gated levels
   come back denied. Returns the URL (0 = ok) or -1. */
int  netease_download_url(const char *song_id, const char *level,
                          char *url, size_t url_sz);

/* Whether the track is purchased/owned (download endpoint's `payed`).
   Level used for the probe (default lossless). Returns 1 = owned,
   0 = not owned (needs purchase / no permission), -1 = probe failed. */
int  netease_song_owned(const char *song_id, const char *level);

/* Refresh the cached purchased-track id list (api/single/mybought/song/
   list, paginated). 0 = ok, -1 = failed. */
int  netease_purchased_refresh(void);

/* Whether a track is in the purchased list. 1 = purchased, 0 = not (list
   loaded), -1 = list not loaded yet / unknown. */
int  netease_is_purchased(const char *song_id);

/* Purchased single-track list (api/single/mybought/song/list) as SongInfo
   rows (songId→id, name→title, artistName→artist, albumName→album,
   picUrl→cover_url). 0 = ok, -1 = failed. */
int  netease_purchased_songs(SongInfo **out, int *count);

/* Purchased digital-album list (api/digitalAlbum/purchased) as SongInfo
   rows (albumId→id, albumName→title, artist.name→artist, cover→cover_url,
   is_playlist=1 so Enter opens the album). 0 = ok, -1 = failed. */
int  netease_purchased_albums(SongInfo **out, int *count);

/* Tracks of an album (weapi/v1/album/{id}) as SongInfo rows. 0 = ok,
   -1 = failed. */
int  netease_album_songs(const char *album_id, SongInfo **out, int *count);

/* Account VIP entitlement for download gating (vip-info). Mirrors the
   client: 0 = none, 1 = black-vinyl VIP, 2 = SVIP, -1 = error. */
int  netease_vip_level(void);

/* ── Lyrics ────────────────────────────────────────── */
/* Fetch lyrics from Netease. buf is allocated/filled with parsed LRC text.
   Returns 0 on success, -1 on error. Caller must free *buf. */
int  netease_lyric(const char *song_id, char **buf);


#ifdef __cplusplus
}
#endif
