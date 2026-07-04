#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include "core/music_source.h"

/* ── Netease API client ───────────────────────────────
 * Thin wrapper over popen("netease-cli ...").
 * The Go binary handles all Netease crypto, cookies, UNM
 * and returns plain JSON via stdout.
 * ──────────────────────────────────────────────────── */

int  netease_api_init(void);     /* check netease-cli exists */
void netease_api_shutdown(void); /* no-op */

/* ── Account ──────────────────────────────────────────── */
const char* netease_account_name(void);     /* cached after login */
unsigned long netease_get_user_id(void);     /* cached after login */
bool netease_is_logged_in(void);             /* cookie file exists */

/* ── Search ───────────────────────────────────────────── */
typedef struct {
    char *id;           /* song id (string) */
    char *title;
    char *artist;
    char *album;
    int   duration_ms;
} NeteaseSongResult;

typedef struct {
    NeteaseSongResult *songs;
    int                count;
} NeteaseSearchResult;

int  netease_search(const char *keyword, int limit, int offset,
                    NeteaseSearchResult *out);
void netease_search_result_free(NeteaseSearchResult *r);

/* ── QR Login ─────────────────────────────────────────── */
/* Step 1: get unikey + QR URL. Returns 0 on success. */
int  netease_qr_get_key(char *out_unikey, size_t unikey_size,
                        char *qr_url, size_t url_size);

/* Render QR code to ASCII string (caller frees). */
char* netease_qr_render(const char *url);

/* Step 2: poll. Returns 0=success(803), 1=waiting(801),
 *        2=expired(800), 3=scanned(802), <0=error.
 * On success, auto-saves cookies internally. */
int  netease_qr_poll(const char *unikey);

/* ── User playlists ───────────────────────────────────── */
typedef struct {
    unsigned long id;
    char         *name;
} NeteasePlaylistItem;

typedef struct {
    NeteasePlaylistItem *items;
    int                  count;
} NeteasePlaylistResult;

/* Get all user playlists. Returns 0 on success. */
int  netease_get_playlists(NeteasePlaylistResult *out);
void netease_playlist_result_free(NeteasePlaylistResult *r);

/* ── Songs from playlists ─────────────────────────────── */
/* Get songs in a playlist. Returns 0 on success. */
int  netease_get_playlist_songs(const char *playlist_id, SearchResult *out);

/* Get liked songs (红心). Returns 0 on success. */
int  netease_get_liked_songs(SearchResult *out);

/* Load daily recommendation or personalized.
 * type: 0=daily, 1=personalized. Returns 0 on success. */
int  netease_load_menu(int type, int limit, SearchResult *out);

/* ── Song URL ──────────────────────────────────────────── */
/* out_url must be >= 1024 bytes. quality: 0=std, 1=high, 2=lossless */
int  netease_get_play_url(const char *song_id, int quality,
                          char *out_url, size_t url_size);

/* ── Placeholder (unused in v2 MVP) ──────────────────── */
int  netease_get_song_detail(const char *song_id,
                             char **title, char **artist, char **album,
                             int *duration_ms);

#ifdef __cplusplus
}
#endif
