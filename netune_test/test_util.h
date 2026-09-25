/* test_util.h — shared helpers for the netune_test regression suite.
 *
 * The suite is deliberately free of any test framework: each test is a
 * program that returns 0 (pass) or 1 (fail), prints one line per check and
 * a final PASS/FAIL. Keeps netune_test linkable against the real core
 * sources without dragging in FTXUI/UI code. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "plugins/decoders/ffmpeg/ffmpeg_stream.h"

#define NT_OK   0
#define NT_FAIL 1

#define NT_EXPECT(cond, ...)                                       \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                          \
            fprintf(stderr, "\n");                                 \
            return NT_FAIL;                                        \
        }                                                          \
    } while (0)

#define NT_CHECK(ok_var, cond, ...)                                \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                          \
            fprintf(stderr, "\n");                                 \
            ok_var = 0;                                            \
        }                                                          \
    } while (0)

/* ── filesystem ─────────────────────────────────────── */

/* Fresh, empty scratch directory unique to this process: /tmp/netune_test_<pid>_<name>.
   The caller owns the returned string (free()). */
char *nt_tmpdir(const char *name);

int     nt_mkdir_p(const char *path);
void    nt_rmtree(const char *path);
int     nt_write_file(const char *path, const void *data, size_t len);
/* malloc'd contents, or NULL. *len_out receives the byte count. */
unsigned char *nt_read_file(const char *path, size_t *len_out);
int64_t nt_file_size(const char *path);
int     nt_files_equal(const char *a, const char *b);

/* ── fixtures ───────────────────────────────────────── */

/* Deterministic 16-bit stereo PCM WAV (sine + envelope), `seconds` long.
   Pure code so the suite has no external fixture dependency. */
int nt_write_wav(const char *path, int seconds, int sample_rate);

/* Transcode `src` to `dst` with the system ffmpeg. 0 = ok, 1 = ffmpeg
   missing (caller should skip the codec variant). */
int nt_transcode(const char *src, const char *dst);

/* Run a test over the fixture matrix: always 20s wav, plus mp3 and m4a when
   the system ffmpeg is available (a container's demuxer positions itself
   differently after probing, which is where the growing/partial read paths
   diverge). `fn` gets (fixture_path, label) and returns NT_OK / NT_FAIL. */
typedef int (*nt_case_fn)(const char *fixture, const char *label);
int nt_run_codec_matrix(const char *scratch_dir, nt_case_fn fn);

/* ── decoding ───────────────────────────────────────── */

/* Decode the whole stream. Returns the frame count, or -1 if the stream
   never produced a frame (a decode failure would otherwise masquerade as
   a legitimately short file). */
int64_t nt_decode_all(FFStream *s);

/* Convenience: open `url` (local path or http URL) and decode it all.
   Returns frames, or -1 when the stream could not be opened. */
int64_t nt_decode_url(const char *url, int *sample_rate, int *channels);

#ifdef __cplusplus
}
#endif
