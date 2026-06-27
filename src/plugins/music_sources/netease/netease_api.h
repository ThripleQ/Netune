#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include "core/music_source.h"

/* ── Netease API client ───────────────────────────────
 * Thin wrapper over libcurl for Netease Cloud Music API.
 * ──────────────────────────────────────────────────── */

/* Initialize the API client (call once).
 * Starts the Node.js API server as a child process on first use. */
int  netease_api_init(void);
void netease_api_shutdown(void);

/* Override the API base URL (default: http://localhost:10000) */
void netease_api_set_base_url(const char *url);

/* ── Search ─────────────────────────────────────────── */
typedef struct {
    char *id;           /* song id (string)             */
    char *title;        /* song name                    */
    char *artist;       /* artist name (first only)     */
    char *album;        /* album name                   */
    int   duration_ms;  /* duration in milliseconds     */
} NeteaseSongResult;

typedef struct {
    NeteaseSongResult *songs;
    int                count;
} NeteaseSearchResult;

/* Search songs by keyword. Returns 0 on success.
   Caller frees result with netease_search_result_free(). */
int netease_search(const char *keyword, int limit, int offset,
                   NeteaseSearchResult *out);

void netease_search_result_free(NeteaseSearchResult *r);

/* ── Login (QR code flow) ───────────────────────────── */
/* Step 1: Get unikey for QR login.
 * out_unikey must be at least 64 bytes. */
int netease_qr_get_key(char *out_unikey, size_t unikey_size, char *qr_url, size_t url_size);

/* Step 2: Poll login status.
 * Returns 0 = logged in, 1 = waiting, 2 = expired, <0 = error.
 * On success reads and stores cookies internally. */
int netease_qr_poll(const char *unikey);

/* Check if currently logged in */
bool netease_is_logged_in(void);

/* Get logged-in account name (borrowed, do not free) */
const char* netease_account_name(void);

/* Get user ID after login (0 if not logged in) */
long netease_get_user_id(void);

/* ── User playlists ──────────────────────────────────── */
typedef struct {
    long   id;
    char  *name;
    int    track_count;
    bool   subscribed;  /* true = favorited, false = created */
} NeteasePlaylist;

typedef struct {
    NeteasePlaylist *items;
    int              count;
} NeteasePlaylistResult;

/* Get user's playlists (requires login).
 * mine_only: 0=all, 1=created only, 2=subscribed only */
int netease_get_playlists(long uid, int mine_only, NeteasePlaylistResult *out);
void netease_playlist_result_free(NeteasePlaylistResult *r);

/* Get songs in a playlist */
int netease_get_playlist_songs(long playlist_id, SearchResult *out);

/* ── Song detail / play URL ─────────────────────────── */
/* Get song detail by id. out fields are allocated strings. */
int netease_get_song_detail(const char *song_id,
                            char **title, char **artist, char **album,
                            int *duration_ms);

/* Load songs for a netease menu item type.
 * type: 0=daily, 1=recommended playlists, 2=my playlists
 * Returns SearchResult (caller must search_result_free). */
int netease_load_menu(int type, int limit, SearchResult *out);

/* Get play URL for a song. out_url must be at least 1024 bytes.
 * quality: 0=standard, 1=high, 2=lossless */
int netease_get_play_url(const char *song_id, int quality,
                         char *out_url, size_t url_size);

/* ── Lyrics / cover ─────────────────────────────────── */
/* Get lyrics for a song. buf must be at least 4096 bytes. */
int netease_get_lyric(const char *song_id, char *buf, size_t buf_size);

/* Get cover URL for a song. buf must be at least 1024 bytes. */
int netease_get_cover_url(const char *song_id, char *buf, size_t buf_size);

/* Get personalized playlists (for menu). Returns raw JSON in out_json. */
int netease_get_personalized(char *out_json, size_t json_size);

#ifdef __cplusplus
}
#endif
