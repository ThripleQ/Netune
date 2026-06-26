#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Loop mode (mirrors C++ LoopMode values 0-2) ── */
#define LOOP_NONE      0
#define LOOP_TRACK     1
#define LOOP_PLAYLIST  2

/* ── API ─────────────────────────────────────────────
 *
 * playlist_manager is the playback decision backend.
 * It owns a copy of song paths (not full metadata) and
 * handles all navigation logic independently from the UI.
 *
 * UI syncs paths here when playing a new playlist.
 * Backend returns decisions (index + path), UI applies them.
 * ──────────────────────────────────────────────── */

int  playlist_manager_init(void);

/* Sync a playlist to the backend. Owners the paths.
   count=0 clears the list. */
void playlist_manager_sync(const char **paths, int count);

int  playlist_manager_count(void);
int  playlist_manager_get_index(void);
void playlist_manager_set_index(int idx);

/* Get the path at given index (owned internally, do not free). */
const char* playlist_manager_get_path(int idx);

void playlist_manager_set_loop_mode(int mode);
int  playlist_manager_get_loop_mode(void);

/* Advance/Retreat: return new index (or -1 if stuck).
   Updates internal index on success. */
int  playlist_manager_advance(void);
int  playlist_manager_retreat(void);

#ifdef __cplusplus
}
#endif
