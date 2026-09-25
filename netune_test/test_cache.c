/* test_cache.c — audio cache index, capacity LRU and crash reconciliation.
 *
 * Covers the parts of docs/cache-redesign.md that are pure bookkeeping and
 * therefore cheap to regression-test: hit/miss by quality, complete vs
 * partial segments, the size cap (LRU by ts), reconcile() after an unclean
 * shutdown, and a full clear(). The cache root is redirected via
 * XDG_CACHE_HOME so the real user cache is never touched. */
#include "test_util.h"
#include "core/audio_cache.h"
#include "core/cache_segments.h"
#include "infra/config.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Create a file of `size` bytes at path inside the audio cache dir. */
static int make_cache_file(const char *name, int64_t size) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", audio_cache_dir(), name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    char buf[4096];
    memset(buf, 'n', sizeof(buf));
    int64_t left = size;
    while (left > 0) {
        size_t n = left < (int64_t)sizeof(buf) ? (size_t)left : sizeof(buf);
        if (fwrite(buf, 1, n, f) != n) { fclose(f); return -1; }
        left -= (int64_t)n;
    }
    fclose(f);
    return 0;
}

static char *cache_file_path(const char *name) {
    static char buf[4096];
    snprintf(buf, sizeof(buf), "%s/%s", audio_cache_dir(), name);
    return buf;
}

int main(void) {
    int ok = 1;
    char *root = nt_tmpdir("cache");
    if (!root) return NT_FAIL;
    setenv("XDG_CACHE_HOME", root, 1);

    /* a config with a 1 MB cap so the LRU test does not write 2 GB */
    char cfg_path[4096];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", root);
    const char *cfg_json =
        "{\"cache\":{\"audio_enabled\":true,\"audio_limit_mb\":1,"
        "\"audio_max_entries\":200}}";
    NT_EXPECT(nt_write_file(cfg_path, cfg_json, strlen(cfg_json)) == 0, "config");
    Config *cfg = config_load(cfg_path);
    NT_EXPECT(cfg != NULL, "config_load failed");
    config_set_global(cfg);

    NT_EXPECT(audio_cache_enabled() == 1, "cache should be enabled");

    /* ── paths ── */
    audio_cache_ensure_dir();
    const char *dir = audio_cache_dir();
    NT_EXPECT(strstr(dir, "netune") != NULL && strstr(dir, "audio") != NULL,
              "unexpected cache dir %s", dir);
    NT_EXPECT(nt_file_size(dir) >= 0, "cache dir was not created");
    char *part = audio_cache_part_path("42");
    NT_EXPECT(part != NULL && strstr(part, "42.part") != NULL,
              "part path %s", part ? part : "(null)");
    free(part);
    char *final = audio_cache_final_path("42", ".mp3");
    NT_EXPECT(final != NULL && strstr(final, "42.mp3") != NULL,
              "final path %s", final ? final : "(null)");
    free(final);

    /* ── complete entry: hit + quality match ── */
    NT_EXPECT(make_cache_file("42.mp3", 100 * 1024) == 0, "create 42.mp3");
    NT_EXPECT(audio_cache_commit("42", cache_file_path("42.mp3"), "lossless", 1, NULL) == 0,
              "commit complete entry");
    char found[4096];
    int complete = -1;
    CacheSegList segs;
    cache_seglist_init(&segs);
    NT_EXPECT(audio_cache_find("42", "lossless", found, sizeof(found), &complete, &segs) == 0,
              "complete entry should be a hit");
    NT_CHECK(ok, complete == 1, "complete entry reported complete=%d", complete);
    NT_CHECK(ok, strcmp(found, cache_file_path("42.mp3")) == 0, "wrong path %s", found);
    NT_CHECK(ok, cache_seglist_total(&segs) == 100 * 1024,
             "complete entry covers %lld bytes", (long long)cache_seglist_total(&segs));
    cache_seglist_free(&segs);
    NT_EXPECT(audio_cache_find("42", "hires", found, sizeof(found), NULL, NULL) == -1,
              "quality mismatch must be a miss");
    NT_EXPECT(audio_cache_find("nope", "lossless", found, sizeof(found), NULL, NULL) == -1,
              "unknown song must miss");

    /* ── partial entry: complete=0 with a two-segment map ── */
    NT_EXPECT(make_cache_file("43.mp3", 200 * 1024) == 0, "create 43.mp3");
    CacheSegList in;
    cache_seglist_init(&in);
    cache_seglist_add(&in, 0, 100 * 1024);
    cache_seglist_add(&in, 150 * 1024, 50 * 1024);
    NT_EXPECT(audio_cache_commit("43", cache_file_path("43.mp3"), "exhigh", 0, &in) == 0,
              "commit partial entry");
    cache_seglist_free(&in);
    complete = -1;
    cache_seglist_init(&segs);
    NT_EXPECT(audio_cache_find("43", "exhigh", found, sizeof(found), &complete, &segs) == 0,
              "partial entry should be a hit");
    NT_CHECK(ok, complete == 0, "partial entry reported complete=%d", complete);
    NT_CHECK(ok, segs.count == 2, "partial entry has %d segments, expected 2", segs.count);
    NT_CHECK(ok, cache_seglist_total(&segs) == 150 * 1024,
             "partial entry covers %lld bytes", (long long)cache_seglist_total(&segs));
    NT_CHECK(ok, cache_seglist_contains(&segs, 120 * 1024) == 0,
             "gap position reported as cached");
    cache_seglist_free(&segs);

    /* ── capacity: 3 x 400 KB against a 1 MB cap evicts the oldest ── */
    audio_cache_clear();   /* isolate the LRU phase from the entries above */
    const char *ids[] = {"lru_a", "lru_b", "lru_c"};
    for (int i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s.mp3", ids[i]);
        NT_EXPECT(make_cache_file(name, 400 * 1024) == 0, "create %s", name);
        NT_EXPECT(audio_cache_commit(ids[i], cache_file_path(name), "standard", 1, NULL) == 0,
                  "commit %s", ids[i]);
        sleep_ms(1100);   /* ts resolution is 1s: keep the LRU order deterministic */
    }
    long long total = audio_cache_total_bytes();
    NT_CHECK(ok, total <= 1024LL * 1024, "cache total %lld exceeds the 1 MB cap",
             (long long)total);
    NT_CHECK(ok, audio_cache_entry_count() == 2, "expected 2 surviving entries, got %d",
             audio_cache_entry_count());
    NT_CHECK(ok, audio_cache_find("lru_c", "standard", found, sizeof(found), NULL, NULL) == 0,
             "newest entry was evicted");
    NT_CHECK(ok, audio_cache_find("lru_a", "standard", found, sizeof(found), NULL, NULL) == -1,
             "oldest entry survived eviction");
    NT_CHECK(ok, nt_file_size(cache_file_path("lru_a.mp3")) < 0,
             "evicted file still on disk");
    NT_CHECK(ok, audio_cache_oldest_ts() > 0, "oldest_ts not reported");

    /* ── reconcile: missing file + stray .part are crash residue ── */
    int before = audio_cache_entry_count();
    NT_EXPECT(unlink(cache_file_path("lru_c.mp3")) == 0, "unlink lru_c");
    NT_EXPECT(make_cache_file("lru_b.mp3.part", 4096) == 0, "create stray .part");
    NT_EXPECT(audio_cache_reconcile() == 0, "reconcile failed");
    NT_CHECK(ok, audio_cache_entry_count() == before - 1,
             "reconcile left %d entries (expected %d)", audio_cache_entry_count(), before - 1);
    NT_CHECK(ok, nt_file_size(cache_file_path("lru_b.mp3.part")) < 0,
             "reconcile left a stray .part behind");

    /* ── clear wipes files + index ── */
    int removed = audio_cache_clear();
    NT_CHECK(ok, removed >= 1, "clear removed %d files", removed);
    NT_CHECK(ok, audio_cache_entry_count() == 0, "index not empty after clear");
    NT_CHECK(ok, audio_cache_total_bytes() == 0, "size not zero after clear");

    config_free(cfg);
    nt_rmtree(root);
    free(root);
    printf("%s\n", ok ? "PASS test_cache" : "FAIL test_cache");
    return ok ? NT_OK : NT_FAIL;
}
