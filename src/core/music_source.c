#include "music_source.h"
#include <stdlib.h>
#include <string.h>

static char* sdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *cpy = (char*)malloc(len);
    if (cpy) memcpy(cpy, s, len);
    return cpy;
}

void song_info_free(SongInfo *info) {
    if (!info) return;
    free(info->id);
    free(info->source);
    free(info->title);
    free(info->artist);
    free(info->album);
    free(info->cover_url);
    free(info->aux_label);
    *info = (SongInfo){0};
}

void song_info_copy(SongInfo *dst, const SongInfo *src) {
    if (!dst || !src) return;
    *dst = *src;
    dst->id        = sdup(src->id);
    dst->source    = sdup(src->source);
    dst->title     = sdup(src->title);
    dst->artist    = sdup(src->artist);
    dst->album     = sdup(src->album);
    dst->cover_url = sdup(src->cover_url);
    dst->aux_label = sdup(src->aux_label);
}

void search_result_free(SearchResult *result) {
    if (!result) return;
    for (int i = 0; i < result->count; i++)
        song_info_free(&result->songs[i]);
    free(result->songs);
    *result = (SearchResult){0};
}
