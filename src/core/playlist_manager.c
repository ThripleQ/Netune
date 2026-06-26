#include "playlist_manager.h"
#include "infra/log.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal state ────────────────────────────────── */
static SongInfo *g_songs = NULL;
static int       g_count = 0;
static int       g_index = -1;   /* -1 = nothing selected */
static int g_loop_mode = LOOP_NONE;
static char     *g_source = NULL; /* e.g. "local" */

int playlist_manager_init(void) {
    LOG_INFO("Playlist manager initialized");
    return 0;
}

void playlist_manager_set_playlist(SongInfo *songs, int count,
                                    const char *source_name) {
    /* free old */
    for (int i = 0; i < g_count; i++)
        song_info_free(&g_songs[i]);
    free(g_songs);
    free(g_source);

    g_songs  = songs;
    g_count  = count;
    g_index  = (count > 0) ? 0 : -1;
    g_source = source_name ? strdup(source_name) : NULL;

    LOG_INFO("Playlist set: %d songs (source: %s)", count,
             g_source ? g_source : "(none)");
}

void playlist_manager_set_loop_mode(int mode) {
    g_loop_mode = mode;
}

int playlist_manager_get_loop_mode(void) {
    return g_loop_mode;
}

int playlist_manager_count(void) {
    return g_count;
}

int playlist_manager_current_index(void) {
    return (g_index >= 0 && g_index < g_count) ? g_index : -1;
}

const SongInfo* playlist_manager_current(void) {
    if (g_index < 0 || g_index >= g_count) return NULL;
    return &g_songs[g_index];
}

const SongInfo* playlist_manager_get(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_songs[idx];
}

void playlist_manager_set_index(int idx) {
    if (idx >= 0 && idx < g_count) g_index = idx;
}

/* ── Advance: EOF / next track ─────────────────────── */
int playlist_manager_advance(void) {
    if (g_count <= 0) return -1;

    switch (g_loop_mode) {
    case LOOP_TRACK:
        /* stay on the same track */;
        return g_index;

    case LOOP_PLAYLIST:
        /* wrap to beginning */
        if (g_index >= g_count - 1) {
            g_index = 0;
            return g_index;
        }
        /* fall through */
    default: /* LOOP_NONE */
        if (g_index >= g_count - 1)
            return -1; /* end of playlist */
        g_index++;
        return g_index;
    }
}

int playlist_manager_retreat(void) {
    if (g_count <= 0) return -1;

    if (g_index > 0) {
        g_index--;
        return g_index;
    }

    /* at first song — wrap only in playlist mode */
    if (g_loop_mode == LOOP_PLAYLIST) {
        g_index = g_count - 1;
        return g_index;
    }

    return -1;
}
