#pragma once

/* ── MPRIS D-Bus service (Linux only) ─────────────────
 * Registers org.mpris.MediaPlayer2.netune on the session
 * bus, exposes Player methods + properties, and emits
 * PropertiesChanged signals.
 *
 * Threading: the D-Bus dispatch runs on its own thread.
 * mpris_sync() must be called from the UI (main) thread
 * once per frame — it snapshots AppState into the MPRIS
 * module under a lock, so the D-Bus thread never touches
 * StateStore directly.
 * No-op on Windows.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WIN32

/* ── External control commands ────────────────────────
   Delivered to the app via EV_MPRIS_COMMAND (int payload). */
enum MprisCommand {
    MPRIS_CMD_PLAYPAUSE = 1,
    MPRIS_CMD_STOP,
    MPRIS_CMD_NEXT,
    MPRIS_CMD_PREV,
    MPRIS_CMD_SEEK,     /* payload = target position in seconds */
};

int  mpris_init(void);
void mpris_shutdown(void);

/* Copy the current playback state into the MPRIS module and
 * emit PropertiesChanged if anything visible changed.
 * state_ptr must stay valid for the duration of the call. */
void mpris_sync(const void *state_ptr);
#endif

#ifdef __cplusplus
}
#endif
