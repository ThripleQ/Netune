#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Playback coordinator ──────────────────────────────
 * Manages decoder + audio output in a background thread.
 * Commands received via event bus.
 * State changes published via event bus.
 */

int  playback_coordinator_init(void);
void playback_coordinator_shutdown(void);

#ifdef __cplusplus
}
#endif
