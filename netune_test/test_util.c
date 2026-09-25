/* test_util.c — see test_util.h. */
#include "test_util.h"

#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── filesystem ─────────────────────────────────────── */

int nt_mkdir_p(const char *path) {
    char buf[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            buf[i] = c;
        }
    }
    return 0;
}

char *nt_tmpdir(const char *name) {
    /* line-buffer stdout: a test that wedges must still show how far it got
       when a runner captures the output through a pipe */
    static int unbuffered = 0;
    if (!unbuffered) { setvbuf(stdout, NULL, _IOLBF, 0); unbuffered = 1; }
    char path[512];
    snprintf(path, sizeof(path), "/tmp/netune_test_%ld_%s", (long)getpid(), name);
    nt_rmtree(path);
    if (nt_mkdir_p(path) != 0) return NULL;
    return strdup(path);
}

static void rmtree_rec(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                char child[4096];
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                rmtree_rec(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

void nt_rmtree(const char *path) { rmtree_rec(path); }

int nt_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = len ? fwrite(data, 1, len, f) : 0;
    int ok = (w == len);
    fclose(f);
    return ok ? 0 : -1;
}

unsigned char *nt_read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = 0;
    if (len_out) *len_out = (size_t)sz;
    return buf;
}

int64_t nt_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int nt_files_equal(const char *a, const char *b) {
    size_t la = 0, lb = 0;
    unsigned char *da = nt_read_file(a, &la);
    unsigned char *db = nt_read_file(b, &lb);
    int eq = (da && db && la == lb && memcmp(da, db, la) == 0);
    free(da);
    free(db);
    return eq;
}

/* ── fixtures ───────────────────────────────────────── */

int nt_write_wav(const char *path, int seconds, int sample_rate) {
    const int channels = 2;
    const int bits = 16;
    int64_t frames = (int64_t)seconds * sample_rate;
    int64_t data_bytes = frames * channels * (bits / 8);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    unsigned char hdr[44];
    uint32_t chunk = 36 + (uint32_t)data_bytes;
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &chunk, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16;
    uint16_t audio_fmt = 1, ch = (uint16_t)channels, bps = (uint16_t)bits;
    uint32_t rate = (uint32_t)sample_rate;
    uint32_t byte_rate = rate * channels * (bits / 8);
    uint16_t block_align = (uint16_t)(channels * (bits / 8));
    uint32_t data_sz = (uint32_t)data_bytes;
    memcpy(hdr + 16, &fmt_size, 4);
    memcpy(hdr + 20, &audio_fmt, 2);
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bps, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_sz, 4);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return -1; }

    /* write in blocks: a two-tone signal with an amplitude envelope, so a
       truncated/misaligned decode is visible in the frame count too */
    enum { BLK = 4096 };
    int16_t buf[BLK * channels];
    int64_t written = 0;
    while (written < frames) {
        int n = (int)((frames - written) < BLK ? (frames - written) : BLK);
        for (int i = 0; i < n; i++) {
            double t = (double)(written + i) / sample_rate;
            double env = 0.35 + 0.65 * fabs(sin(2.0 * M_PI * 0.25 * t));
            double s = sin(2.0 * M_PI * 440.0 * t) + 0.5 * sin(2.0 * M_PI * 660.0 * t);
            int16_t v = (int16_t)(s * env * 12000.0);
            buf[i * 2 + 0] = v;
            buf[i * 2 + 1] = (int16_t)(v / 2);
        }
        if (fwrite(buf, sizeof(int16_t) * channels, (size_t)n, f) != (size_t)n) {
            fclose(f);
            return -1;
        }
        written += n;
    }
    fclose(f);
    return 0;
}

int nt_transcode(const char *src, const char *dst) {
    char cmd[4096];
    /* -nostdin: never steal the parent's stdin; -loglevel error keeps the
       test output readable. Returns non-zero when ffmpeg is missing. */
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -nostdin -loglevel error -y -i '%s' '%s' >/dev/null 2>&1",
             src, dst);
    if (system("command -v ffmpeg >/dev/null 2>&1") != 0) return 1;
    return system(cmd) == 0 ? 0 : 1;
}

int nt_run_codec_matrix(const char *scratch_dir, nt_case_fn fn) {
    char wav[4096];
    snprintf(wav, sizeof(wav), "%s/source.wav", scratch_dir);
    if (nt_write_wav(wav, 20, 44100) != 0) {
        fprintf(stderr, "fixture generation failed\n");
        return NT_FAIL;
    }
    int rc = fn(wav, "wav");
    static const char *exts[] = {"mp3", "m4a"};
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        char out[4096];
        snprintf(out, sizeof(out), "%s/source.%s", scratch_dir, exts[i]);
        if (nt_transcode(wav, out) == 0) rc |= fn(out, exts[i]);
        else printf("  %s: skipped (ffmpeg not available)\n", exts[i]);
    }
    return rc;
}

/* ── decoding ───────────────────────────────────────── */

int64_t nt_decode_all(FFStream *s) {
    if (!s) return -1;
    static int16_t pcm[4096 * 2];
    int64_t total = 0;
    for (;;) {
        int n = ffstream_decode(s, pcm, 4096);
        if (n <= 0) break;
        total += n;
        if (total > 44100LL * 3600) break;   /* 1h guard: runaway decode */
    }
    return total;
}

int64_t nt_decode_url(const char *url, int *sample_rate, int *channels) {
    int sr = 0, ch = 0, dur = 0;
    FFStream *s = ffstream_open(url, &sr, &ch, &dur);
    if (!s) return -1;
    int64_t frames = nt_decode_all(s);
    ffstream_close(s);
    if (sample_rate) *sample_rate = sr;
    if (channels) *channels = ch;
    return frames;
}
