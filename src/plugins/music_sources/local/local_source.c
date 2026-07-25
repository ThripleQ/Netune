#include "local_source.h"
#include "core/decoder_manager.h"
#include "core/music_source_manager.h"
#include "infra/log.h"
#include "infra/config.h"
#include "infra/path.h"
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
    a->items    = NULL;
    a->count    = 0;
    a->capacity = 0;
}

static void song_array_push(SongArray *a, const SongInfo *s) {
    if (a->count >= a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 64;
        a->items = (SongInfo*)realloc(a->items,
                      (size_t)a->capacity * sizeof(SongInfo));
    }
    int idx = a->count++;
    a->items[idx] = *s;
    a->items[idx].id        = strdup(s->id);
    a->items[idx].source    = strdup(s->source);
    a->items[idx].title     = strdup(s->title);
    a->items[idx].artist    = strdup(s->artist);
    a->items[idx].album     = strdup(s->album);
    a->items[idx].cover_url = strdup(s->cover_url);
    a->items[idx].aux_label = strdup(s->aux_label);
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
            scan_dir(full, arr);
        } else if (S_ISREG(st.st_mode) && has_music_ext(entry->d_name)) {
            SongInfo s = {0};
            s.id     = strdup(full);
            s.source = strdup("local");
            s.title  = strdup(entry->d_name);
            s.artist = strdup("");
            s.album  = strdup("");

            Decoder *d = decoder_open(full);
            if (d) {
                DecoderInfo info;
                decoder_get_info(d, &info);
                if (info.total_frames > 0 && info.sample_rate > 0)
                    s.duration_sec = info.total_frames / info.sample_rate;
                decoder_close(d);
            }

            song_array_push(arr, &s);
        }
    }
    closedir(dir);
}

/* ── Global song cache ────────────────────────────────
 * Scans configured dirs once, then serves searches from cache.
 * If no dirs are configured, the cache stays empty. */
static SongArray g_all_songs = {0};
static bool      g_scanned = false;

/* Match a single song against keyword (title / artist / album / filename) */
static bool song_matches(const SongInfo *s, const char *keyword) {
    if (!s || !keyword) return false;
    const char *title  = s->title  ? s->title  : "";
    const char *artist = s->artist ? s->artist : "";
    const char *album  = s->album  ? s->album  : "";
    const char *fname  = s->id ? strrchr(s->id, '/') : NULL;
    fname = fname ? fname + 1 : (s->id ? s->id : "");
    return strcasestr(title, keyword)  ||
           strcasestr(artist, keyword) ||
           strcasestr(album, keyword)  ||
           strcasestr(fname, keyword);
}

/* Scan configured dirs into the global cache. Idempotent. */
static void ensure_cache(void) {
    if (g_scanned) return;
    Config *cfg = config_global();
    int ndirs = cfg ? config_get_array_size(cfg, "music_sources.local.dirs") : 0;
    if (ndirs <= 0) {
        LOG_WARN("No music_sources.local.dirs configured — local cache empty. "
                 "Configure dirs in data/config.json to enable local music.");
    } else {
        for (int i = 0; i < ndirs; i++) {
            char key[64];
            snprintf(key, sizeof(key), "music_sources.local.dirs[%d]", i);
            const char *raw = config_get_str(cfg, key, NULL);
            if (!raw) continue;
            char *dir = path_expand(raw);
            scan_dir(dir, &g_all_songs);
            free(dir);
        }
    }
    LOG_INFO("Local source cache: %d songs scanned", g_all_songs.count);
    g_scanned = true;
}

/* Filter the global cache by keyword, return matching songs. */
static int search_filtered(const char *keyword, SongArray *out) {
    song_array_init(out);

    /* No keyword → return everything */
    if (!keyword || !keyword[0]) {
        for (int i = 0; i < g_all_songs.count; i++)
            song_array_push(out, &g_all_songs.items[i]);
        return 0;
    }

    /* Filter from cache */
    for (int i = 0; i < g_all_songs.count; i++) {
        if (song_matches(&g_all_songs.items[i], keyword))
            song_array_push(out, &g_all_songs.items[i]);
    }
    return 0;
}

/* ── MusicSource implementation ──────────────────────── */
static int local_init(void) {
    ensure_cache();
    LOG_INFO("Local music source initialized");
    return 0;
}

static void local_shutdown(void) {
    for (int i = 0; i < g_all_songs.count; i++)
        song_info_free(&g_all_songs.items[i]);
    free(g_all_songs.items);
    g_all_songs.items    = NULL;
    g_all_songs.count    = 0;
    g_all_songs.capacity = 0;
    g_scanned = false;
    LOG_INFO("Local music source shutdown");
}

static int local_search(const char *keyword, int page, int page_size,
                        SearchResult *out) {
    (void)page;
    (void)page_size;
    if (!out) return -1;

    ensure_cache();
    SongArray arr;
    search_filtered(keyword, &arr);

    out->songs = arr.items;
    out->count = arr.count;
    out->total = arr.count;
    return 0;
}

static int local_get_song_detail(const char *song_id, SongInfo *out) {
    Decoder *d = decoder_open(song_id);
    if (!d) return -1;

    DecoderInfo info;
    decoder_get_info(d, &info);

    const char *fname = strrchr(song_id, '/');
    fname = fname ? fname + 1 : song_id;

    out->id     = strdup(song_id);
    out->source = strdup("local");
    out->title  = strdup(fname);
    out->artist = strdup("");
    out->album  = strdup("");
    if (info.total_frames > 0 && info.sample_rate > 0)
        out->duration_sec = info.total_frames / info.sample_rate;

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
    return 0;
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
    .name            = "local",
    .priority        = 10,
    .init            = local_init,
    .shutdown        = local_shutdown,
    .search          = local_search,
    .get_song_detail = local_get_song_detail,
    .get_play_url    = local_get_play_url,
    .get_lyric       = local_get_lyric,
    .get_cover_url   = local_get_cover_url,
    .is_available    = local_is_available,
};

void local_source_register(void) {
    music_source_register(&g_local_source);
}

MusicSource* local_source_create(void) {
    return &g_local_source;
}
