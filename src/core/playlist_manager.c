#include "playlist_manager.h"
#include "infra/log.h"

/* ── Internal state — NO song data, only navigation ── */
static int g_count      = 0;
static int g_index      = -1;   /* -1 = nothing selected */
static int g_loop_mode  = LOOP_NONE;

int playlist_manager_init(void) {
    LOG_INFO("Playlist manager initialized");
    return 0;
}

void playlist_manager_set_count(int count) {
    g_count = (count > 0) ? count : 0;
    if (g_index >= g_count) g_index = g_count > 0 ? 0 : -1;
}

int playlist_manager_get_count(void) {
    return g_count;
}

void playlist_manager_set_index(int idx) {
    if (idx >= 0 && idx < g_count) g_index = idx;
}

int playlist_manager_get_index(void) {
    return (g_index >= 0 && g_index < g_count) ? g_index : -1;
}

void playlist_manager_set_loop_mode(int mode) {
    g_loop_mode = mode;
}

int playlist_manager_get_loop_mode(void) {
    return g_loop_mode;
}

int playlist_manager_advance(void) {
    if (g_count <= 0) return -1;

    switch (g_loop_mode) {
    case LOOP_TRACK:
        return g_index;  /* same track */
    case LOOP_PLAYLIST:
        if (g_index >= g_count - 1) {
            g_index = 0;  /* wrap */
            return g_index;
        }
        /* fall through */
    default: /* LOOP_NONE */
        if (g_index >= g_count - 1)
            return -1;  /* end */
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

    if (g_loop_mode == LOOP_PLAYLIST && g_count > 0) {
        g_index = g_count - 1;  /* wrap to end */
        return g_index;
    }

    return -1;  /* at start, no wrap */
}
