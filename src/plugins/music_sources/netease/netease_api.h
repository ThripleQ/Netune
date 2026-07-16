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
typedef struct { char *id, *title, *artist, *album; int dur_ms; } NSSong;
typedef struct { NSSong *songs; int count; } NSSearchResult;

int  netease_search(const char *kw, int limit, int offset, NSSearchResult *out);
void netease_search_free(NSSearchResult *r);

/* ── Login ─────────────────────────────────────────── */
int  netease_qr_key(char *unikey, size_t uk_sz, char *url, size_t url_sz);
char* netease_qr_render(const char *url);
int  netease_qr_poll(const char *unikey);
bool netease_is_logged_in(void);

/* ── Playlists ─────────────────────────────────────── */
int  netease_playlists(bool favorited, SongInfo **out, int *count);
int  netease_playlist_songs(const char *id,        /* songs in a playlist */
                            SongInfo **out, int *count);
int  netease_liked_songs(SongInfo **out, int *count);   /* liked songs */
int  netease_menu_songs(int type, int limit,            /* daily etc */
                        SongInfo **out, int *count);

/* ── Play URL + Download ──────────────────────────── */
/* Get streaming URL.                                      */
int  netease_play_url(const char *song_id, char *url, size_t url_sz);

/* ── Lyrics ────────────────────────────────────────── */
/* Fetch lyrics from Netease. buf is allocated/filled with parsed LRC text.
   Returns 0 on success, -1 on error. Caller must free *buf. */
int  netease_lyric(const char *song_id, char **buf);


#ifdef __cplusplus
}
#endif
