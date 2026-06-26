#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "music_source.h"
#include <stdbool.h>

/* ── Loop mode (mirrors C++ LoopMode values 0-2) ── */
#define LOOP_NONE      0
#define LOOP_TRACK     1
#define LOOP_PLAYLIST  2

/* ── API ───────────────────────────────────────────── */
int  playlist_manager_init(void);

/* Replace entire playlist (takes ownership of songs array) */
void playlist_manager_set_playlist(SongInfo *songs, int count,
                                   const char *source_name);
void playlist_manager_set_loop_mode(int mode);
int  playlist_manager_get_loop_mode(void);

/* Playlist navigation */
int  playlist_manager_count(void);
int  playlist_manager_current_index(void);
const SongInfo* playlist_manager_current(void);
const SongInfo* playlist_manager_get(int idx);
void playlist_manager_set_index(int idx);

/* Advance: returns the index of the next track to play.
   Returns -1 if there is no next track (playlist empty or end reached
   with LOOP_NONE). Updates internal index. */
int  playlist_manager_advance(void);

/* Retreat (prev), similar logic. Returns -1 if cannot go back. */
int  playlist_manager_retreat(void);

#ifdef __cplusplus
}
#endif
