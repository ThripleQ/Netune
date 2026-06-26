#include "local_source.h"
#include "core/decoder_manager.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Dynamic array helpers ──────────────────────────── */
typedef struct {
    SongInfo *items;
    int       count;
    int       capacity;
} SongArray;

static void song_array_init(SongArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

static char* sdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *cpy = (char*)malloc(len);
    if (cpy) memcpy(cpy, s, len);
    return cpy;
}

static void song_array_push(SongArray *a, const SongInfo *s) {
    if (a->count >= a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 64;
        a->items = (SongInfo*)realloc(a->items,
                      (size_t)a->capacity * sizeof(SongInfo));
    }
    /* shallow copy, then deep-dup strings */
    int idx = a->count++;
    a->items[idx] = *s;
    a->items[idx].id        = sdup(s->id);
    a->items[idx].source    = sdup(s->source);
    a->items[idx].title     = sdup(s->title);
    a->items[idx].artist    = sdup(s->artist);
    a->items[idx].album     = sdup(s->album);
    a->items[idx].cover_url = sdup(s->cover_url);
    a->items[idx].aux_label = sdup(s->aux_label);
}

static void song_array_free(SongArray *a) {
    for (int i = 0; i < a->count; i++)
        song_info_free(&a->items[i]);
    free(a->items);
    a->items = NULL;
    a->count = a->capacity = 0;
}

/* ── File scanning ──────────────────────────────────── */
static bool has_music_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return decoder_supports_ext(dot + 1);
}

static void scan_dir(const char *dir_path, SongArray *arr) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir(full, arr); /* recurse */
        } else if (S_ISREG(st.st_mode) && has_music_ext(entry->d_name)) {
            SongInfo s = {0};
            s.id   = strdup(full);
            s.source = strdup("local");
            s.title  = strdup(entry->d_name);
            s.artist = strdup("");
            s.album  = strdup("");

            /* probe duration via decoder */
            Decoder *d = decoder_open(full);
            if (d) {
                DecoderInfo info;
                decoder_get_info(d, &info);
                if (info.total_frames > 0 && info.samplerate > 0)
                    s.duration_sec = info.total_frames / info.samplerate;
                decoder_close(d);
            }

            song_array_push(arr, &s);
        }
    }
    closedir(dir);
}

/* ── MusicSource implementation ──────────────────────── */
static int local_init(void) {
    LOG_INFO("Local music source initialized");
    return 0;
}

static void local_shutdown(void) {
    LOG_INFO("Local music source shutdown");
}

static int local_search(const char *keyword, int page, int page_size,
                        SearchResult *out) {
    /* For local files, "keyword" is the directory path to scan */
    if (!keyword || !out) return -1;
    SongArray arr;
    song_array_init(&arr);
    scan_dir(keyword, &arr);

    out->songs = arr.items;
    out->count = arr.count;
    out->total = arr.count;
    /* song_array items are now owned by caller */
    return 0;
}

static int local_get_song_detail(const char *song_id, SongInfo *out) {
    /* For local files, song_id is the file path */
    Decoder *d = decoder_open(song_id);
    if (!d) return -1;

    DecoderInfo info;
    decoder_get_info(d, &info);

    const char *fname = strrchr(song_id, '/');
    fname = fname ? fname + 1 : song_id;

    /* fill basic info */
    free(out->id); out->id = strdup(song_id);
    free(out->source); out->source = strdup("local");
    free(out->title);  out->title  = strdup(fname);
    if (info.total_frames > 0 && info.samplerate > 0)
        out->duration_sec = info.total_frames / info.samplerate;

    decoder_close(d);
    return 0;
}

static int local_get_play_url(const char *song_id, int quality,
                              char *url, size_t url_size) {
    (void)quality;
    snprintf(url, url_size, "file://%s", song_id);
    return 0;
}

static int local_get_lyric(const char *song_id, char *buf, size_t buf_size) {
    (void)song_id;
    if (buf_size > 0) buf[0] = '\0';
    return 0; /* no lyrics for local files */
}

static int local_get_cover_url(const char *song_id, char *buf, size_t buf_size) {
    (void)song_id;
    if (buf_size > 0) buf[0] = '\0';
    return 0;
}

static bool local_is_available(void) {
    return true;
}

static MusicSource g_local_source = {
    .name           = "local",
    .priority       = 10,
    .init           = local_init,
    .shutdown       = local_shutdown,
    .search         = local_search,
    .get_song_detail = local_get_song_detail,
    .get_play_url   = local_get_play_url,
    .get_lyric      = local_get_lyric,
    .get_cover_url  = local_get_cover_url,
    .is_available   = local_is_available,
};

void local_source_register(void) {
    /* just expose the static instance */
    LOG_INFO("Local source plugin ready");
}

MusicSource* local_source_create(void) {
    return &g_local_source;
}
