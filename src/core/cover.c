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

/* ── Nearest-neighbor downscale ──────────────────── */
static void cover_scale(const uint8_t *src, int sw, int sh, int ch,
                        uint8_t *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            int si = (sy * sw + sx) * ch;
            int di = (y * dw + x) * ch;
            for (int c = 0; c < ch; c++)
                dst[di + c] = src[si + c];
        }
    }
}

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

    /* Store at a reasonable max. The renderer will scale on-the-fly per frame. */
    int max_dim = 200;
    int dw = (w > h) ? max_dim : (max_dim * w / h);
    int dh = (h > w) ? max_dim : (max_dim * h / w);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    uint8_t *scaled = (uint8_t*)malloc((size_t)dw * dh * 3);
    if (!scaled) {
        stbi_image_free(pixels);
        return -1;
    }
    cover_scale(pixels, w, h, 3, scaled, dw, dh);
    stbi_image_free(pixels);

    out->pixels   = scaled;
    out->width    = dw;
    out->height   = dh;
    out->channels = 3;
    return 0;
}

/* ── Free ────────────────────────────────────────── */
void cover_free(CoverData *cd) {
    if (!cd) return;
    free(cd->pixels);
    memset(cd, 0, sizeof(*cd));
}
