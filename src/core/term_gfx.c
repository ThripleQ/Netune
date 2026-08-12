#include "core/term_gfx.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* No known Windows console implements the kitty graphics protocol. */
TermGfxMode term_gfx_detect(void) {
    return TERM_GFX_NONE;
}
int term_gfx_active(void) { return 0; }
void term_gfx_ensure(const CoverData *cd, int cols, int rows) {
    (void)cd; (void)cols; (void)rows;
}
unsigned long term_gfx_image_id(void) { return 0; }
#else

/* ── Protocol state ─────────────────────────────────── */
static TermGfxMode g_mode = TERM_GFX_NONE;
static int g_detected = 0;

/* fingerprint of the image currently uploaded */
static const uint8_t *g_uploaded_pixels = NULL;
static int g_uploaded_w = 0;
static int g_uploaded_h = 0;

/* stable id handed out to the terminal (kitty i= parameter) */
static unsigned long g_img_id = 0;

/* ── Base64 (RFC 4648) ──────────────────────────────── */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_size(size_t in) {
    return ((in + 2) / 3) * 4;
}

static void b64_encode(const uint8_t *src, size_t n, char *out) {
    size_t i = 0, o = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8) |
                     src[i+2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >>  6) & 63];
        out[o++] = B64[v & 63];
        i += 3;
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >>  6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* ── Raw escape output ───────────────────────────────── */
static void esc_write(const char *s) {
    fwrite(s, 1, strlen(s), stdout);
}

/* ── Detection ───────────────────────────────────────── */
TermGfxMode term_gfx_detect(void) {
    if (g_detected) return g_mode;
    g_detected = 1;

    const char *kitty_env = getenv("KITTY_WINDOW_ID");
    const char *term = getenv("TERM");
    /* kitty, wezterm, ghostty and foot (1.14+) all set KITTY_WINDOW_ID;
       fall back to TERM strings for exotic setups. */
    if (kitty_env && kitty_env[0]) {
        g_mode = TERM_GFX_KITTY;
    } else if (term && (strstr(term, "kitty") || strstr(term, "foot"))) {
        g_mode = TERM_GFX_KITTY;
    }
    if (g_mode != TERM_GFX_NONE)
        LOG_INFO("Terminal graphics: kitty protocol");
    return g_mode;
}

int term_gfx_active(void) {
    return term_gfx_detect() == TERM_GFX_KITTY;
}

unsigned long term_gfx_image_id(void) {
    return term_gfx_active() ? g_img_id : 0;
}

/* ── Upload (chunked a=T transfer) ────────────────────── */
static void kitty_upload_raw(const uint8_t *rgb, int w, int h,
                             unsigned long id) {
    size_t total = (size_t)w * h * 3;
    size_t b64_total = b64_size(total);
    char *b64 = (char*)malloc(b64_total + 1);
    if (!b64) { LOG_ERROR("term_gfx: OOM"); return; }
    b64_encode(rgb, total, b64);

    const size_t CHUNK = 3000;  /* base64 chars per transmission chunk */
    char header[128];
    for (size_t off = 0; off < b64_total; off += CHUNK) {
        size_t n = b64_total - off;
        int last = (n <= CHUNK);
        if (n > CHUNK) n = CHUNK;
        if (off == 0) {
            snprintf(header, sizeof(header),
                     "\x1b_Ga=T,i=%lu,f=24,s=%d,v=%d,m=%d;",
                     id, w, h, last ? 0 : 1);
        } else {
            snprintf(header, sizeof(header), "\x1b_Gm=%d;", last ? 0 : 1);
        }
        esc_write(header);
        fwrite(b64 + off, 1, n, stdout);
        esc_write("\x1b\\");
    }
    free(b64);
}

void term_gfx_ensure(const CoverData *cd, int cols, int rows) {
    if (!term_gfx_active() || !cd || !cd->pixels) return;
    if (cols < 1 || rows < 1) return;

    /* re-transfer when the image changed */
    if (g_uploaded_pixels != cd->pixels ||
        g_uploaded_w != cd->width || g_uploaded_h != cd->height) {
        g_img_id++;
        if (g_img_id == 0) g_img_id = 1;  /* id 0 is invalid */
        kitty_upload_raw(cd->pixels, cd->width, cd->height, g_img_id);
        g_uploaded_pixels = cd->pixels;
        g_uploaded_w = cd->width;
        g_uploaded_h = cd->height;
        LOG_DEBUG("term_gfx: uploaded cover %dx%d id=%lu",
                  cd->width, cd->height, g_img_id);
    }

    /* (re)create the virtual placement matching the rendered grid so the
       placeholder cells know the image's target size */
    static int s_vp_cols = -1, s_vp_rows = -1;
    if (s_vp_cols != cols || s_vp_rows != rows) {
        char seq[128];
        /* a=p place, U=1 virtual placement (invisible prototype) */
        snprintf(seq, sizeof(seq),
                 "\x1b_Ga=p,U=1,i=%lu,q=2,c=%d,r=%d\x1b\\",
                 g_img_id, cols, rows);
        esc_write(seq);
        s_vp_cols = cols;
        s_vp_rows = rows;
        LOG_DEBUG("term_gfx: virtual placement %dx%d id=%lu",
                  cols, rows, g_img_id);
    }
}

#endif /* _WIN32 */
