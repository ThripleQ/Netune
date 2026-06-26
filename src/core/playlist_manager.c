#include "playlist_manager.h"
#include "infra/log.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal state: owns paths, not metadata ───────── */
static char      **g_paths   = NULL;
static int         g_count   = 0;
static int         g_index   = -1;
static int         g_loop_mode = LOOP_NONE;

int playlist_manager_init(void) {
    LOG_INFO("Playlist manager initialized");
    return 0;
}

static void free_paths(void) {
    for (int i = 0; i < g_count; i++)
        free(g_paths[i]);
    free(g_paths);
    g_paths = NULL;
    g_count = 0;
    g_index = -1;
}

void playlist_manager_sync(const char **paths, int count) {
    free_paths();

    if (count <= 0 || !paths) return;

    g_paths = (char**)calloc((size_t)count, sizeof(char*));
    if (!g_paths) return;

    for (int i = 0; i < count; i++)
        g_paths[i] = paths[i] ? strdup(paths[i]) : NULL;

    g_count = count;
    g_index = 0;
}

int playlist_manager_count(void) {
    return g_count;
}

int playlist_manager_get_index(void) {
    return (g_index >= 0 && g_index < g_count) ? g_index : -1;
}

void playlist_manager_set_index(int idx) {
    if (idx >= 0 && idx < g_count) g_index = idx;
}

const char* playlist_manager_get_path(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return g_paths[idx];
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
        return g_index;
    case LOOP_PLAYLIST:
        if (g_index >= g_count - 1) {
            g_index = 0;
            return g_index;
        }
        /* fall through */
    default:
        if (g_index >= g_count - 1) return -1;
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
        g_index = g_count - 1;
        return g_index;
    }

    return -1;
}
