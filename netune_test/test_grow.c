/* test_grow.c — M1 growing-file decode (ffstream_open_growing).
 *
 * Regression for the exact bug the handoff calls out: a growing file that is
 * appended to by a background writer must decode to the same frame count as
 * the finished file, and the read callback must return AVERROR_EOF (negative)
 * at the tail — returning 0 made FFmpeg's avio_read bypass path spin forever.
 *
 * The writer here is a thread appending from the source file, mirroring the
 * watermark/condvar protocol of stream_downloader (test_download.c exercises
 * the real downloader end to end). */
#include "test_util.h"
#include "core/cache_segments.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char src[4096];
    char dst[4096];
    int64_t total;
    int64_t watermark;
    int done;    /* writer finished */
    int failed;
    pthread_mutex_t lock;
    pthread_cond_t cv;
} Grower;

static void *grower_main(void *arg) {
    Grower *g = (Grower *)arg;
    FILE *in = fopen(g->src, "rb");
    int out = in ? open(g->dst, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    char buf[64 * 1024];
    if (in && out >= 0) {
        for (;;) {
            size_t n = fread(buf, 1, sizeof(buf), in);
            if (n == 0) break;
            size_t off = 0;
            while (off < n) {
                ssize_t w = write(out, buf + off, n - off);
                if (w <= 0) { n = 0; break; }
                off += (size_t)w;
            }
            pthread_mutex_lock(&g->lock);
            g->watermark += (int64_t)off;
            pthread_cond_broadcast(&g->cv);
            pthread_mutex_unlock(&g->lock);
            struct timespec ts = {0, 2 * 1000 * 1000};   /* 2ms: keep the reader waiting */
            nanosleep(&ts, NULL);
        }
    }
    pthread_mutex_lock(&g->lock);
    if (!in || out < 0) g->failed = 1;
    g->done = 1;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->lock);
    if (in) fclose(in);
    if (out >= 0) close(out);
    return NULL;
}

/* blocking read callback: waits until `pos + want` bytes are on disk */
static int grow_wait(void *opaque, int64_t pos, int want) {
    Grower *g = (Grower *)opaque;
    pthread_mutex_lock(&g->lock);
    for (;;) {
        if (g->failed) { pthread_mutex_unlock(&g->lock); return -1; }
        if (g->watermark >= pos + want) {                 /* enough for this read */
            pthread_mutex_unlock(&g->lock);
            return 1;                                     /* retry the read */
        }
        if (g->watermark >= g->total) {                   /* finished: drain + EOF */
            pthread_mutex_unlock(&g->lock);
            return 0;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g->cv, &g->lock, &ts);
    }
    pthread_mutex_unlock(&g->lock);
    return 1;
}

static int run_case(const char *fixture, const char *label) {
    int ok = 1;
    int64_t expect = nt_decode_url(fixture, NULL, NULL);
    NT_EXPECT(expect > 0, "%s: static decode produced no frames", label);

    char *dir = nt_tmpdir("grow");
    NT_EXPECT(dir != NULL, "tmpdir");
    char dst[4096];
    snprintf(dst, sizeof(dst), "%s/growing.%s", dir, label);

    Grower g;
    memset(&g, 0, sizeof(g));
    snprintf(g.src, sizeof(g.src), "%s", fixture);
    snprintf(g.dst, sizeof(g.dst), "%s", dst);
    g.total = nt_file_size(fixture);
    pthread_mutex_init(&g.lock, NULL);
    pthread_cond_init(&g.cv, NULL);

    /* create an empty file first so ffstream_open_growing can fopen("rb") */
    NT_EXPECT(nt_write_file(dst, "", 0) == 0, "%s: create growing file", label);
    pthread_t th;
    NT_EXPECT(pthread_create(&th, NULL, grower_main, &g) == 0, "spawn writer");

    int sr = 0, ch = 0, dur = 0;
    FFStream *s = ffstream_open_growing(dst, grow_wait, &g, g.total, &sr, &ch, &dur);
    NT_EXPECT(s != NULL, "%s: ffstream_open_growing failed", label);
    NT_EXPECT(ffstream_growing(s) == 1, "%s: not reported as growing", label);
    int64_t got = nt_decode_all(s);
    ffstream_close(s);
    pthread_join(th, NULL);

    NT_CHECK(ok, got == expect,
             "%s: growing decode %lld frames != static %lld",
             label, (long long)got, (long long)expect);
    NT_CHECK(ok, !g.failed, "%s: writer failed", label);
    NT_CHECK(ok, g.watermark == g.total, "%s: writer wrote %lld/%lld bytes",
             label, (long long)g.watermark, (long long)g.total);
    NT_CHECK(ok, nt_files_equal(dst, fixture),
             "%s: growing copy is not byte-identical to the source", label);
    printf("  %s: %lld frames (static %lld)\n",
           label, (long long)got, (long long)expect);
    pthread_mutex_destroy(&g.lock);
    pthread_cond_destroy(&g.cv);
    free(dir);
    return ok ? NT_OK : NT_FAIL;
}

int main(void) {
    char *dir = nt_tmpdir("grow_fixtures");
    if (!dir) return NT_FAIL;
    int rc = nt_run_codec_matrix(dir, run_case);
    nt_rmtree(dir);
    free(dir);
    printf("%s\n", rc == NT_OK ? "PASS test_grow" : "FAIL test_grow");
    return rc;
}
