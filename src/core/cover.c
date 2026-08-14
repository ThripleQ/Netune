#include "core/cover.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define POPEN  _popen
  #define PCLOSE _pclose
#else
  #define POPEN  popen
  #define PCLOSE pclose
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ── Area-average (box) downscale ─────────────────────
   Every destination pixel averages the full source rectangle it maps to
   (contiguous floor boundaries: no gaps, no overlap). Unlike the old
   nearest-neighbor sampling this keeps thin outlines and high-contrast
   edges visible when shrinking large covers, because no source pixel is
   ever dropped. */
static void cover_scale(const uint8_t *src, int sw, int sh, int ch,
                        uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh;
        int y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw;
            int x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            long acc[4] = {0, 0, 0, 0};
            long n = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint8_t *row = src + (size_t)sy * sw * ch;
                for (int sx = x0; sx < x1; sx++) {
                    const uint8_t *p = row + sx * ch;
                    for (int c = 0; c < ch; c++)
                        acc[c] += p[c];
                    n++;
                }
            }
            if (n == 0) n = 1;
            uint8_t *o = dst + ((size_t)y * dw + x) * ch;
            for (int c = 0; c < ch; c++)
                o[c] = (uint8_t)(acc[c] / n);
        }
    }
}

/* unique id source for CoverData.stamp */
static uint64_t g_cover_seq = 0;

/* ── Run a program and capture stdout ────────────── */
static char *popen_read(const char *cmd, size_t *out_size) {
    FILE *fp = POPEN(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 65536, len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) { PCLOSE(fp); return NULL; }
    while (!feof(fp)) {
        if (len + 4096 >= cap) {
            cap *= 2;
            char *t = (char*)realloc(buf, cap);
            if (!t) { free(buf); PCLOSE(fp); return NULL; }
            buf = t;
        }
        size_t r = fread(buf + len, 1, cap - len - 1, fp);
        if (r > 0) len += r; else break;
    }
    buf[len] = '\0';
    PCLOSE(fp);
    if (out_size) *out_size = len;
    return buf;
}

/* ── shell-escape a string for safe popen embedding ── */
static char *shell_escape(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
#ifndef _WIN32
    /* POSIX: wrap in single quotes, escape embedded quotes as '\'' */
    char *out = (char*)malloc(len * 4 + 3);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            out[j++] = '\''; out[j++] = '\\'; out[j++] = '\''; out[j++] = '\'';
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
#else
    /* Windows: wrap in double quotes, escape embedded double quotes */
    char *out = (char*)malloc(len * 2 + 3);
    if (!out) return NULL;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') { out[j++] = '\\'; out[j++] = '"'; }
        else if (s[i] == '^' || s[i] == '&' || s[i] == '|' ||
                 s[i] == '<' || s[i] == '>' || s[i] == '%') {
            out[j++] = '^'; out[j++] = s[i];
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
#endif
}

/* ── Load cover from URL ─────────────────────────── */
int cover_load(const char *url, CoverData *out) {
    if (!url || !out) return -1;

    memset(out, 0, sizeof(*out));

    /* Download image data — shell-escape the URL to prevent injection */
    char *esc = shell_escape(url);
    if (!esc) return -1;
    char cmd[4096];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "curl -sL --max-time 10 %s 2>NUL", esc);
#else
    snprintf(cmd, sizeof(cmd), "curl -sL --max-time 10 %s 2>/dev/null", esc);
#endif
    free(esc);
    size_t img_size = 0;
    char *img_data = popen_read(cmd, &img_size);
    if (!img_data || img_size == 0) {
        LOG_WARN("cover: download failed for %s", url);
        free(img_data);
        return -1;
    }

    /* Decode with stb_image */
    int w, h, ch;
    uint8_t *pixels = stbi_load_from_memory(
        (const stbi_uc*)img_data, (int)img_size, &w, &h, &ch, 3);  /* force RGB */
    free(img_data);

    if (!pixels) {
        LOG_WARN("cover: decode failed: %s", stbi_failure_reason());
        return -1;
    }

    /* Store the original image (within a sanity cap). The renderer samples
       from it per frame, so keeping full resolution preserves detail —
       2048px covers real-world covers (typically 300–800px) while capping
       memory against 4K+ edge cases.

       NOTE: images *below* the cap keep their native size. The previous
       code resized everything to a 2048px longest side, nearest-neighbor
       UPSCALING typical 300–800px covers: ~12MB of duplicated pixels per
       cover, a ~17MB base64 kitty upload, and zero extra detail. */
    int max_dim = 2048;
    int dw = w, dh = h;
    if (w > max_dim || h > max_dim) {
        if (w >= h) { dw = max_dim; dh = max_dim * h / w; }
        else        { dh = max_dim; dw = max_dim * w / h; }
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    uint8_t *scaled = (uint8_t*)malloc((size_t)dw * dh * 3);
    if (!scaled) {
        stbi_image_free(pixels);
        return -1;
    }
    if (dw == w && dh == h)
        memcpy(scaled, pixels, (size_t)w * h * 3);
    else
        cover_scale(pixels, w, h, 3, scaled, dw, dh);
    stbi_image_free(pixels);

    out->pixels   = scaled;
    out->width    = dw;
    out->height   = dh;
    out->channels = 3;
    out->stamp    = ++g_cover_seq;
    return 0;
}

/* ── Free ────────────────────────────────────────── */
void cover_free(CoverData *cd) {
    if (!cd) return;
    free(cd->pixels);
    memset(cd, 0, sizeof(*cd));
}

/* ── Terminal cell metrics ────────────────────────── */
static int g_cell_w = 8, g_cell_h = 16;  /* fallback: classic 2:1 */

int cover_cell_width(void)  { return g_cell_w; }
int cover_cell_height(void) { return g_cell_h; }

/* Probe the terminal's cell size in pixels. Sends CSI 14t (pixel size)
   and 18t (text rows/cols), reads the responses from stdin with a
   timeout. Must run before FTXUI enters raw mode. Non-fatal: keeps the
   2:1 default on terminals that don't answer. */
#ifndef _WIN32
#include <termios.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
void cover_cell_probe(void) {
#ifndef _WIN32
    if (!isatty(STDIN_FILENO)) return;

    struct termios old_tio, raw_tio;
    if (tcgetattr(STDIN_FILENO, &old_tio) != 0) return;
    raw_tio = old_tio;
    raw_tio.c_lflag &= ~(ICANON | ECHO);
    raw_tio.c_cc[VMIN] = 0;
    raw_tio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio);

    int old_fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, old_fl | O_NONBLOCK);

    /* "\x1b[14t\x1b[18t" is 10 bytes — write() the full query, otherwise
       the 18t half is truncated and rows/cols never arrive, silently
       falling back to the 2:1 default and distorting the cover */
    const char query[] = "\x1b[14t\x1b[18t";
    write(STDOUT_FILENO, query, sizeof(query) - 1);
    fflush(stdout);

    int px_w = 0, px_h = 0, rows = 0, cols = 0;
    char buf[128];
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    for (int i = 0; i < 20 && (px_h == 0 || rows == 0); i++) {
        if (poll(&pfd, 1, 50) <= 0) continue;
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (r <= 0) continue;
        buf[r] = '\0';
        /* responses: CSI 4;<h>;<w>t  and  CSI 8;<rows>;<cols>t */
        char *p = buf;
        while ((p = strchr(p, '\x1b')) != NULL) {
            if (p[1] == '[') {
                int a = 0, b = 0;
                if (sscanf(p + 2, "4;%d;%dt", &a, &b) == 2) {
                    px_h = a; px_w = b;
                } else if (sscanf(p + 2, "8;%d;%dt", &a, &b) == 2) {
                    rows = a; cols = b;
                }
            }
            p++;
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    fcntl(STDIN_FILENO, F_SETFL, old_fl);

    if (px_w > 0 && px_h > 0 && rows > 0 && cols > 0) {
        g_cell_w = px_w / cols;
        g_cell_h = px_h / rows;
        if (g_cell_w < 1) g_cell_w = 1;
        if (g_cell_h < 1) g_cell_h = 1;
        LOG_INFO("Terminal cell: %d x %d px (probed)", g_cell_w, g_cell_h);
    }
#endif
}
