#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Playback coordinator ──────────────────────────────
 * Manages decoder + audio output in a background thread.
 * Commands received via event bus.
 * State changes published via event bus.
 */

/* Payload for EV_PLAYBACK_RELOAD: reopen `id` at the currently-resolved
   quality and resume from seek_sec seconds (in-place quality switch). */
typedef struct {
    char id[128];
    int  seek_sec;
} PlaybackReloadCmd;

int  playback_coordinator_init(void);
void playback_coordinator_shutdown(void);

#ifdef __cplusplus
}
#endif
