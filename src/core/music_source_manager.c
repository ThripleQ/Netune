#include "music_source_manager.h"
#include "infra/log.h"
#include <stdlib.h>
#include <string.h>

#define MAX_SOURCES 8

static MusicSource *g_sources[MAX_SOURCES];
static int          g_count = 0;

int music_source_register(MusicSource *source) {
    if (!source || !source->name) return -1;
    if (g_count >= MAX_SOURCES) return -1;

    /* check duplicate */
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_sources[i]->name, source->name) == 0)
            return 0; /* already registered */
    }

    g_sources[g_count++] = source;
    if (source->init) source->init();
    LOG_INFO("Music source registered: %s", source->name);
    return 0;
}

MusicSource* music_source_get(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_sources[i]->name, name) == 0)
            return g_sources[i];
    }
    return NULL;
}

int music_source_search(const char *source_name,
                        const char *keyword, int page, int page_size,
                        SearchResult *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    if (source_name) {
        MusicSource *src = music_source_get(source_name);
        if (!src || !src->search) return -1;
        return src->search(keyword, page, page_size, out);
    }

    /* search all sources, merge results */
    /* For now, just use the first one that returns results */
    for (int i = 0; i < g_count; i++) {
        if (!g_sources[i]->search) continue;
        SearchResult tmp = {0};
        if (g_sources[i]->search(keyword, page, page_size, &tmp) == 0
            && tmp.count > 0) {
            *out = tmp;
            return 0;
        }
    }
    return -1;
}

int music_source_manager_init(void) {
    LOG_INFO("Music source manager initialized (%d sources)", g_count);
    return 0;
}

void music_source_manager_shutdown(void) {
    for (int i = 0; i < g_count; i++) {
        if (g_sources[i]->shutdown)
            g_sources[i]->shutdown();
    }
    LOG_INFO("Music source manager shutdown");
}

int music_source_count(void) {
    return g_count;
}
