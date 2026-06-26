#include "music_source.h"
#include <stdlib.h>

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

void search_result_free(SearchResult *result) {
    if (!result) return;
    for (int i = 0; i < result->count; i++) {
        song_info_free(&result->songs[i]);
    }
    free(result->songs);
    *result = (SearchResult){0};
}
