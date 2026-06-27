#include "search_manager.h"
#include "cache_manager.h"
#include "infra/log.h"
#include "core/event_bus.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Internal state ─────────────────────────────────── */
static char          g_keyword[256] = {0};
static int           g_page = 0;
static SearchResult  g_results = {0};
static bool          g_inited = false;

/* ── Result helpers ──────────────────────────────────── */
static void free_results(void) {
    if (g_results.songs) {
        search_result_free(&g_results);
        g_results.songs = NULL;
    }
    g_results.count = 0;
    g_results.total = 0;
}

/* ── Deduplication ─────────────────────────────────────
 * Check if a song with the same source:id already exists in results. */
static bool has_duplicate(const SongInfo *song) {
    if (!song || !song->id || !song->source) return false;
    for (int i = 0; i < g_results.count; i++) {
        if (g_results.songs[i].id && g_results.songs[i].source &&
            strcmp(g_results.songs[i].id, song->id) == 0 &&
            strcmp(g_results.songs[i].source, song->source) == 0)
            return true;
    }
    return false;
}

/* ── Search across all sources ───────────────────────── */
static int search_all(const char *keyword, int page, int page_size) {
    /* Temporary buffer for aggregated results */
    SongInfo *buf = calloc((size_t)(page_size * 4), sizeof(SongInfo));
    int count = 0;
    int total = 0;

    /* Try cache first */
    if (page == 0) {
        SearchResult cached = {0};
        if (cache_search(keyword, page_size, &cached) == 0 && cached.count > 0) {
            for (int i = 0; i < cached.count; i++) {
                song_info_copy(&buf[count++], &cached.songs[i]);
                song_info_free(&cached.songs[i]);
            }
            free(cached.songs);
            total = cached.total > 0 ? cached.total : count;
        }
    }

    /* Search all registered music sources */
    int n_src = music_source_count();
    for (int si = 0; si < n_src; si++) {
        MusicSource *src = music_source_get_by_index(si);
        if (!src || !src->search) continue;
        if (!src->is_available || !src->is_available()) continue;

        SearchResult sr = {0};
        int rc = src->search(keyword, page, page_size * 2, &sr);
        if (rc != 0 || sr.count <= 0) {
            if (sr.songs) free(sr.songs);
            continue;
        }

        total += sr.total > 0 ? sr.total : sr.count;

        for (int j = 0; j < sr.count && count < page_size * 4; j++) {
            if (!has_duplicate(&sr.songs[j])) {
                song_info_copy(&buf[count++], &sr.songs[j]);
            }
            song_info_free(&sr.songs[j]);
        }
        free(sr.songs);

        /* Cache results for local sources */
        if (src->name && strcmp(src->name, "local") == 0) {
            (void)src; /* cache already handled by local source */
        }
    }

    /* Store in results */
    free_results();
    g_results.songs = calloc((size_t)count, sizeof(SongInfo));
    for (int i = 0; i < count; i++) {
        song_info_copy(&g_results.songs[i], &buf[i]);
        song_info_free(&buf[i]);
    }
    free(buf);
    g_results.count = count;
    g_results.total = total;

    LOG_INFO("Search for '%s': %d results (total: %d)", keyword, count, total);
    return count > 0 ? 0 : -1;
}

/* ── Public API ──────────────────────────────────────── */
int search_manager_init(void) {
    if (g_inited) return 0;
    g_inited = true;
    LOG_INFO("Search manager initialized");
    return 0;
}

int search_manager_search(const char *keyword, int page) {
    if (!keyword || keyword[0] == '\0') {
        search_manager_clear();
        return 0;
    }

    snprintf(g_keyword, sizeof(g_keyword), "%s", keyword);
    g_page = page;

    /* Publish search start event */
    event_bus_publish(EV_SEARCH_START, (void*)keyword, strlen(keyword) + 1);

    /* Execute search */
    int rc = search_all(keyword, page, SEARCH_PAGE_SIZE);

    if (rc == 0) {
        /* Publish results */
        event_bus_publish(EV_SEARCH_RESULT, NULL, 0);
    } else {
        free_results();
        event_bus_publish(EV_SEARCH_ERROR, NULL, 0);
    }

    return rc;
}

const SearchResult* search_manager_results(void) {
    return &g_results;
}

const char* search_manager_keyword(void) {
    return g_keyword;
}

int search_manager_page(void) {
    return g_page;
}

int search_manager_total_results(void) {
    return g_results.total;
}

void search_manager_clear(void) {
    free_results();
    g_keyword[0] = '\0';
    g_page = 0;
}

void search_manager_shutdown(void) {
    free_results();
    g_inited = false;
    LOG_INFO("Search manager shutdown");
}
