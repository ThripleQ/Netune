/* test_stream.c — plain network decode through the local Range server.
 *
 * Baseline for the other streaming tests: ffstream_open() over HTTP must
 * decode to the same frame count as the local file, so a later mismatch in
 * the partial/growing tests is attributable to those paths. */
#include "test_util.h"
#include "http_range_server.h"

#include <stdlib.h>

static int run_case(const char *fixture, const char *label) {
    int ok = 1;
    int64_t expect = nt_decode_url(fixture, NULL, NULL);
    NT_EXPECT(expect > 0, "%s: local decode produced no frames", label);

    HttpRangeServer *srv = hrs_start(fixture);
    NT_EXPECT(srv != NULL, "%s: could not start the range server", label);
    char url[256];
    hrs_url(srv, url, sizeof(url));

    int sr = 0, ch = 0;
    int64_t got = nt_decode_url(url, &sr, &ch);
    int64_t dur = 0;
    NT_CHECK(ok, got == expect, "%s: network decode %lld frames != local %lld",
             label, (long long)got, (long long)expect);
    NT_CHECK(ok, sr > 0 && ch > 0, "%s: stream reported no audio params", label);
    NT_CHECK(ok, hrs_request_count(srv) >= 1, "%s: no request served", label);
    printf("  %s: %lld frames, %d Hz / %d ch, %d requests\n",
           label, (long long)got, sr, ch, hrs_request_count(srv));
    hrs_stop(srv);
    (void)dur;
    return ok ? NT_OK : NT_FAIL;
}

int main(void) {
    char *dir = nt_tmpdir("stream_fixtures");
    if (!dir) return NT_FAIL;
    int rc = nt_run_codec_matrix(dir, run_case);
    nt_rmtree(dir);
    free(dir);
    printf("%s\n", rc == NT_OK ? "PASS test_stream" : "FAIL test_stream");
    return rc;
}
