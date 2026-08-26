#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* ── Event types ─────────────────────────────────────── */
typedef enum {
    /* playback */
    EV_PLAYBACK_START,
    EV_PLAYBACK_PAUSE,
    EV_PLAYBACK_RESUME,
    EV_PLAYBACK_STOP,
    EV_PLAYBACK_FINISH,
    EV_PLAYBACK_ERROR,
    EV_PLAYBACK_SKIP,  /* song unplayable (no copyright/url) — auto next */
    /* reopen the current netease track at a new quality (per-song override
       or global default changed): payload = PlaybackReloadCmd { id, seek_sec }
       — re-resolves the URL, reopens the stream, seeks and resumes in place */
    EV_PLAYBACK_RELOAD,
    EV_PROGRESS_UPDATE,
    EV_BUFFERING_UPDATE,

    /* playlist */
    EV_PLAYLIST_CHANGED,
    EV_TRACK_CHANGED,
    EV_PLAYLIST_LOADED,
    EV_MENU_LOADED,
    EV_PLAYLIST_LIST_LOADED,

    /* volume */
    EV_VOLUME_CHANGED,
    EV_MUTE_CHANGED,

    /* metadata */
    EV_METADATA_LOADED,
    EV_LYRIC_LOADED,

    /* search */
    EV_SEARCH_START,
    EV_SEARCH_RESULT,
    EV_SEARCH_ERROR,
    EV_SEARCH_DONE,

    /* config */
    EV_CONFIG_CHANGED,
    EV_THEME_CHANGED,

    /* spectrum */
    EV_SPECTRUM_UPDATE,

    /* cover art */
    EV_COVER_LOADED,

    /* a cover requested via cover_cache (song list) finished loading.
       Payload: CoverCacheResult {id, CoverData cd}; the main thread
       calls cover_cache_store() to move the pixels into the cache. */
    EV_COVER_CACHE_LOADED,

    /* system */
    EV_APP_STARTUP,
    EV_APP_SHUTDOWN,

    /* MPRIS external control commands (payload: int MprisCommand) */
    EV_MPRIS_COMMAND,

    /* local music source: rescan dirs + refresh UI groups
       (payload: none) */
    EV_LOCAL_REFRESH,

    /* download queue state changed: enqueue / progress / completion.
       Payload: none → mirror active tasks to StateStore; a C-string → also
       set the app notice (download result). */
    EV_DOWNLOAD_UPDATE,

    /* netease search results posted back to the main thread
       (payload: SearchLoadResult { query, songs, count }) — the main
       thread writes g_ns_cache, so the cache is never touched from the
       search worker thread */
    EV_SEARCH_LOADED,

    EV_COUNT  /* sentinel */
} EventType;

/* ── Event data ──────────────────────────────────────── */
typedef struct BusEvent {
    EventType type;
    void     *data;
    size_t    data_size;    /* bytes, 0 = no payload */
    int       ref_count;    /* internal */
} BusEvent;

/* ── Callback type ────────────────────────────────────── */
typedef void (*EventCallback)(const BusEvent *event, void *user_data);

/* ── API ──────────────────────────────────────────────── */
int  event_bus_init(void);
void event_bus_shutdown(void);
int  event_bus_subscribe(EventType type, EventCallback cb, void *user_data);
int  event_bus_publish(EventType type, void *data, size_t data_size);
void event_bus_poll(void);   /* dispatch queued events (call from main thread) */

#ifdef __cplusplus
}
#endif
