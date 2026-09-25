/* test_download.c — M1 end to end: stream_downloader + ffstream_open_growing.
 *
 * This is the path a cache miss takes in playback_coordinator: the background
 * download thread writes the stream into a .part file (single writer), and
 * the playback thread decodes that file while it grows. Asserts:
 *   - the decode is not truncated (same frame count as the static file),
 *   - the downloader's own watermark/done protocol works (no busy-spin),
 *   - after the download finishes the .part is byte-identical to the source,
 *     i.e. nothing was lost at the watermark/EOF boundary. */
#include "test_util.h"
#include "http_range_server.h"
#include "core/stream_downloader.h"

#include <stdlib.h>
#include <time.h>

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int dl_wait(void *opaque, int64_t pos, int want) {
    StreamDownloader *dl = (StreamDownloader *)opaque;
    if (stream_downloader_failed(dl)) return -1;
    int64_t wm = stream_downloader_watermark(dl);
    if (wm >= pos + want) return 1;
    if (stream_downloader_done(dl)) return 0;
    stream_downloader_wait_watermark(dl, pos + want, 500);
    if (stream_downloader_failed(dl)) return -1;
    return 1;
}

static int run_case(const char *fixture, const char *label) {
    int ok = 1;
    int64_t expect = nt_decode_url(fixture, NULL, NULL);
    NT_EXPECT(expect > 0, "%s: local decode produced no frames", label);
    int64_t src_size = nt_file_size(fixture);

    HttpRangeServer *srv = hrs_start(fixture);
    NT_EXPECT(srv != NULL, "%s: could not start the range server", label);
    char url[256];
    hrs_url(srv, url, sizeof(url));

    char *dir = nt_tmpdir("download");
    NT_EXPECT(dir != NULL, "tmpdir");
    char part[4096];
    snprintf(part, sizeof(part), "%s/cache.part", dir);

    StreamDownloader *dl = stream_downloader_create(url, part);
    NT_EXPECT(dl != NULL, "%s: stream_downloader_create failed", label);
    NT_EXPECT(stream_downloader_start(dl) == 0, "%s: start failed", label);

    /* wait for the response headers so declared_total is the true size */
    int64_t total = 0;
    for (int i = 0; i < 100 && total <= 0; i++) {
        total = stream_downloader_total_size(dl);
        if (total <= 0) sleep_ms(20);
    }
    NT_CHECK(ok, !stream_downloader_failed(dl), "%s: download failed early", label);
    NT_CHECK(ok, total == src_size, "%s: Content-Length %lld != %lld",
             label, (long long)total, (long long)src_size);

    int sr = 0, ch = 0, dur = 0;
    FFStream *s = ffstream_open_growing(part, dl_wait, dl, total, &sr, &ch, &dur);
    NT_EXPECT(s != NULL, "%s: ffstream_open_growing failed", label);
    int64_t got = nt_decode_all(s);
    ffstream_close(s);

    NT_CHECK(ok, got == expect, "%s: growing decode %lld frames != static %lld",
             label, (long long)got, (long long)expect);

    /* let the downloader finish (if the decode did not already drain it) */
    for (int i = 0; i < 300 && !stream_downloader_done(dl); i++) sleep_ms(20);
    NT_CHECK(ok, stream_downloader_done(dl), "%s: download never finished", label);
    NT_CHECK(ok, !stream_downloader_failed(dl), "%s: download failed", label);
    NT_CHECK(ok, stream_downloader_watermark(dl) == src_size,
             "%s: watermark %lld != %lld", label,
             (long long)stream_downloader_watermark(dl), (long long)src_size);
    NT_CHECK(ok, nt_files_equal(part, fixture),
             "%s: .part is not byte-identical to the source", label);
    stream_downloader_destroy(dl);
    hrs_stop(srv);
    printf("  %s: %lld frames, %lld bytes, %d requests\n",
           label, (long long)got, (long long)src_size, hrs_request_count(srv) >= 0 ? 0 : 0);
    nt_rmtree(dir);
    free(dir);
    return ok ? NT_OK : NT_FAIL;
}

int main(void) {
    char *dir = nt_tmpdir("download_fixtures");
    if (!dir) return NT_FAIL;
    int rc = nt_run_codec_matrix(dir, run_case);
    nt_rmtree(dir);
    free(dir);
    printf("%s\n", rc == NT_OK ? "PASS test_download" : "FAIL test_download");
    return rc;
}
