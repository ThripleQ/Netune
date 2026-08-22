#include "local_source.h"
#include "core/decoder_manager.h"
#include "core/music_source_manager.h"
#include "infra/log.h"
#include "infra/config.h"
#include "infra/path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat/utf8.h"
#include <strings.h>
#include <dirent.h>
#include <pthread.h>

/* ── Cross-platform path helpers ────────────────────── */
#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

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
        int newcap = a->capacity ? a->capacity * 2 : 64;
        SongInfo *items = (SongInfo*)realloc(a->items,
                          (size_t)newcap * sizeof(SongInfo));
        if (!items) return;  /* keep existing array; skip this entry */
        a->items    = items;
        /* Zero the newly-grown region: song_info_copy() frees dst's old
           strings first, and stale garbage pointers from realloc() would
           crash with free(): invalid pointer. */
        for (int i = a->count; i < newcap; i++)
            memset(&items[i], 0, sizeof(SongInfo));
        a->capacity = newcap;
    }
    /* Deep copy via the NULL-safe helper — raw strdup(s->cover_url) would
       crash on the NULL cover_url/aux_label fields produced by scan_dir. */
    song_info_copy(&a->items[a->count++], s);
}

/* ── File scanning ──────────────────────────────────── */
static bool has_music_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return decoder_supports_ext(dot + 1);
}

/* Find the filename portion of a path, handling both / and \ separators. */
static const char *path_basename(const char *path) {
    if (!path) return NULL;
    const char *base = path;
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *last = (slash && bslash) ? (slash > bslash ? slash : bslash)
                     : slash ? slash : bslash;
    if (last) base = last + 1;
    return base;
}

/* Probe one audio file (path already validated as regular + music ext):
   decode it for duration and append to the array. */
static void scan_file(const char *full, const char *name, SongArray *arr) {
    SongInfo s = {0};
    s.id     = strdup(full);
    s.source = strdup("local");
    s.title  = strdup(name);
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
    song_info_free(&s);  /* push deep-copied s; free its scratch strings */
}

static void scan_dir(const char *dir_path, SongArray *arr) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        /* Skip hidden entries (.git, dotfiles, ...) — they are never
           music and often contain thousands of files (e.g. downloaded
           themes/checkouts). */
        if (entry->d_name[0] == '.')
            continue;

        char full[2048];
        snprintf(full, sizeof(full), "%s" PATH_SEP "%s", dir_path, entry->d_name);

        unsigned char dt = entry->d_type;
        if (dt == DT_LNK)
            continue;  /* never follow symlinks (no cycle risk, themes etc.) */
        if (dt == DT_DIR) {
            scan_dir(full, arr);
        } else if (dt == DT_REG && has_music_ext(entry->d_name)) {
            scan_file(full, entry->d_name, arr);
        } else if (dt == DT_UNKNOWN) {
            /* filesystems without d_type: fall back to stat() */
            struct stat st;
            if (stat_utf8(full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                scan_dir(full, arr);
            } else if (S_ISREG(st.st_mode) && has_music_ext(entry->d_name)) {
                scan_file(full, entry->d_name, arr);
            }
        }
    }
    closedir(dir);
}

/* ── Global song cache ────────────────────────────────
 * Scans configured dirs once, then serves searches from cache.
 * If no dirs are configured, the cache stays empty. */
static SongArray g_all_songs = {0};
static bool      g_scanned = false;
/* Guard the cache: download completion (background thread) rescans the
   dirs while the UI thread may be reading g_all_songs. */
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Match a single song against keyword (title / artist / album / filename) */
static bool song_matches(const SongInfo *s, const char *keyword) {
    if (!s || !keyword) return false;
    const char *title  = s->title  ? s->title  : "";
    const char *artist = s->artist ? s->artist : "";
    const char *album  = s->album  ? s->album  : "";
    const char *fname  = s->id ? path_basename(s->id) : "";
    return strcasestr(title, keyword)  ||
           strcasestr(artist, keyword) ||
           strcasestr(album, keyword)  ||
           strcasestr(fname, keyword);
}

/* Scan configured dirs into the global cache. Idempotent. */
static void ensure_cache(void) {
    pthread_mutex_lock(&g_cache_mutex);
    if (g_scanned) { pthread_mutex_unlock(&g_cache_mutex); return; }
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
    pthread_mutex_unlock(&g_cache_mutex);
}

/* Filter the global cache by keyword, return matching songs. */
static int search_filtered(const char *keyword, SongArray *out) {
    song_array_init(out);

    pthread_mutex_lock(&g_cache_mutex);
    /* No keyword → return everything */
    if (!keyword || !keyword[0]) {
        for (int i = 0; i < g_all_songs.count; i++)
            song_array_push(out, &g_all_songs.items[i]);
        pthread_mutex_unlock(&g_cache_mutex);
        return 0;
    }

    /* Filter from cache */
    for (int i = 0; i < g_all_songs.count; i++) {
        if (song_matches(&g_all_songs.items[i], keyword))
            song_array_push(out, &g_all_songs.items[i]);
    }
    pthread_mutex_unlock(&g_cache_mutex);
    return 0;
}

/* ── MusicSource implementation ──────────────────────── */
static int local_init(void) {
    ensure_cache();
    LOG_INFO("Local music source initialized");
    return 0;
}

static void local_shutdown(void) {
    pthread_mutex_lock(&g_cache_mutex);
    for (int i = 0; i < g_all_songs.count; i++)
        song_info_free(&g_all_songs.items[i]);
    free(g_all_songs.items);
    g_all_songs.items    = NULL;
    g_all_songs.count    = 0;
    g_all_songs.capacity = 0;
    g_scanned = false;
    pthread_mutex_unlock(&g_cache_mutex);
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

    const char *fname = path_basename(song_id);

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

int local_register_download_dir(const char *dir) {
    if (!dir || !dir[0]) return -1;
    Config *cfg = config_global();
    if (!cfg) return -1;

    /* Guard the check-then-push against concurrent download threads
       (two downloads finishing at the same time would both pass the
       "already configured?" check and append the dir twice). */
    pthread_mutex_lock(&g_cache_mutex);

    char *exp = path_expand(dir);
    const char *want = exp ? exp : dir;

    /* Already configured? (compare against expanded entries) */
    int ndirs = config_get_array_size(cfg, "music_sources.local.dirs");
    for (int i = 0; i < ndirs; i++) {
        char key[64];
        snprintf(key, sizeof(key), "music_sources.local.dirs[%d]", i);
        const char *raw = config_get_str(cfg, key, NULL);
        if (!raw) continue;
        char *e = path_expand(raw);
        bool same = e && strcmp(e, want) == 0;
        free(e);
        if (same) {
            free(exp);
            pthread_mutex_unlock(&g_cache_mutex);
            /* Already configured — still rescan: the cache only holds the
               first scan, so a later download would otherwise never appear
               in the local list until the app restarts. (local_shutdown and
               ensure_cache take the mutex themselves; we must not hold it
               across them.) */
            local_shutdown();
            ensure_cache();
            return 0;
        }
    }

    if (!config_array_push_str(cfg, "music_sources.local.dirs", want)) {
        free(exp);
        pthread_mutex_unlock(&g_cache_mutex);
        return -1;
    }
    config_save(cfg);
    free(exp);
    pthread_mutex_unlock(&g_cache_mutex);

    /* Force a rescan so new downloads appear in the local list at once.
       (local_shutdown/ensure_cache take the mutex themselves.) */
    local_shutdown();
    ensure_cache();
    return 0;
}
