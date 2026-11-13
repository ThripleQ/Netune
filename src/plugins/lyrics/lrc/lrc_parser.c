#include "core/lyric.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat/utf8.h"   /* UTF-8 aware fopen for Windows */

/* ── Parse a single LRC timestamp line ──────────────── */
/* Returns number of lines parsed from this one string. */

/*
 * LRC format: [mm:ss.xx]text  or  [mm:ss]text
 * Multiple timestamps per line: [00:01.00][00:30.00]text
 *
 * The fraction is centiseconds by convention, but files in the wild use
 * 1–3 digits; interpret by digit count instead of assuming hundredths
 * (a naive "%d" made ".52" 52 ms instead of 520 ms).
 */
static int parse_line(const char *line, LyricLine *out, int cap) {
    int n = 0;
    const char *p = line;
    int times[64]; /* max 64 timestamps per line */
    int nt = 0;

    while (*p) {
        if (*p == '[') {
            const char *q = p + 1;
            int m = 0, s = 0, used = 0;
            if (sscanf(q, "%d:%d%n", &m, &s, &used) == 2) {
                q += used;
                int frac = 0, fd = 0;
                if (*q == '.') {
                    q++;
                    while (*q >= '0' && *q <= '9') {
                        if (fd < 9) frac = frac * 10 + (*q - '0');
                        fd++;
                        q++;
                    }
                }
                /* skip to the closing bracket */
                while (*q && *q != ']') q++;
                if (*q == ']' && nt < 64) {
                    int ms;
                    if (fd == 0)      ms = 0;
                    else if (fd == 1) ms = frac * 100;
                    else if (fd == 2) ms = frac * 10;
                    else {
                        for (; fd > 3; fd--) frac /= 10;
                        ms = frac;
                    }
                    if (m >= 0 && s >= 0 && ms >= 0)
                        times[nt++] = (m * 60 + s) * 1000 + ms;
                }
                p = (*q == ']') ? q + 1 : q;
                continue;
            }
        }
        break;
    }

    /* skip whitespace at start of text */
    while (*p == ' ' || *p == '\t') p++;

    if (nt == 0 || !*p) return 0;

    /* find end of text (strip trailing whitespace/newlines) */
    const char *end = p + strlen(p);
    while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        end--;

    for (int i = 0; i < nt && n < cap; i++) {
        out[n].time_ms = times[i];
        size_t len = (size_t)(end - p);
        out[n].text = (char*)malloc(len + 1);
        if (out[n].text) {
            memcpy(out[n].text, p, len);
            out[n].text[len] = '\0';
        } else {
            out[n].text = strdup("");
        }
        n++;
    }
    return n;
}

/* ── Sort helper ───────────────────────────────────── */
static int cmp_line(const void *a, const void *b) {
    return ((const LyricLine*)a)->time_ms - ((const LyricLine*)b)->time_ms;
}

/* ── Parse full LRC text ───────────────────────────── */
Lyrics* lyric_parse(const char *lrc_text) {
    if (!lrc_text || !lrc_text[0]) return NULL;

    /* UTF-8 BOM would hide the first "[mm:ss]" from the parser. */
    if ((unsigned char)lrc_text[0] == 0xEF &&
        (unsigned char)lrc_text[1] == 0xBB &&
        (unsigned char)lrc_text[2] == 0xBF)
        lrc_text += 3;

    int capacity = 256;
    LyricLine *lines = (LyricLine*)calloc((size_t)capacity, sizeof(LyricLine));
    if (!lines) return NULL;
    int count = 0;

    const char *p = lrc_text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        /* One source line can expand to up to 64 timestamped lines; grow
           instead of silently truncating long lyrics at 256 entries. */
        if (count + 64 > capacity) {
            int ncap = capacity * 2;
            while (ncap < count + 64) ncap *= 2;
            LyricLine *grown = (LyricLine*)realloc(
                lines, (size_t)ncap * sizeof(LyricLine));
            if (!grown) break;
            memset(grown + capacity, 0,
                   (size_t)(ncap - capacity) * sizeof(LyricLine));
            lines = grown;
            capacity = ncap;
        }

        /* copy line to tmp buffer */
        char *tmp = (char*)malloc(len + 1);
        if (!tmp) break;
        memcpy(tmp, p, len);
        tmp[len] = '\0';

        int parsed = parse_line(tmp, lines + count, capacity - count);
        count += parsed;

        free(tmp);

        if (!nl) break;
        p = nl + 1;
    }

    if (count == 0) {
        free(lines);
        return NULL;
    }

    /* sort by timestamp */
    qsort(lines, (size_t)count, sizeof(LyricLine), cmp_line);

    Lyrics *ly = (Lyrics*)malloc(sizeof(Lyrics));
    if (!ly) {
        for (int i = 0; i < count; i++) free(lines[i].text);
        free(lines);
        return NULL;
    }
    ly->lines = lines;
    ly->count = count;
    return ly;
}

/* ── Find active line ──────────────────────────────── */
int lyric_find_line(const Lyrics *ly, int time_ms) {
    if (!ly || ly->count == 0) return -1;

    /* binary search for the rightmost line with time_ms <= current */
    int lo = 0, hi = ly->count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ly->lines[mid].time_ms <= time_ms) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

/* ── Load LRC from file ──────────────────────────── */
Lyrics* lrc_load_file(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    /* Reject empty files and runaway "lyrics" (e.g. a video misnamed .lrc). */
    if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);

    char *buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    Lyrics *ly = lyric_parse(buf);
    free(buf);
    return ly;
}

/* ── Free ───────────────────────────────────────────── */
void lyric_free(Lyrics *ly) {
    if (!ly) return;
    for (int i = 0; i < ly->count; i++)
        free(ly->lines[i].text);
    free(ly->lines);
    free(ly);
}
