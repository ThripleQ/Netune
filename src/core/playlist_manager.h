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
 * playlist_manager is a lightweight navigation helper.
 * It ONLY manages index and loop_mode — it does NOT
 * store song data. Song data lives in StateStore (the
 * single source of truth).
 *
 * Call flow:
 *   idx = playlist_manager_advance();
 *   if (idx >= 0) {
 *       song = StateStore.playlist[idx];  // read data
 *       play(song);
 *   }
 * ──────────────────────────────────────────────── */

int  playlist_manager_init(void);

void playlist_manager_set_count(int count);
int  playlist_manager_get_count(void);

/* Current index */
void playlist_manager_set_index(int idx);
int  playlist_manager_get_index(void);

/* Loop mode */
void playlist_manager_set_loop_mode(int mode);
int  playlist_manager_get_loop_mode(void);

/* Navigation: return next/prev index, or -1 if cannot move.
   Updates internal index on success. */
int  playlist_manager_advance(void);
int  playlist_manager_retreat(void);

#ifdef __cplusplus
}
#endif
