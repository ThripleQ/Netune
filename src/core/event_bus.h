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

    /* cover art */
    EV_COVER_LOADED,

    /* system */
    EV_APP_STARTUP,
    EV_APP_SHUTDOWN,

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
