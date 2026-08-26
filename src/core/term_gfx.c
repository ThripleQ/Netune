#include "core/term_gfx.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* No known Windows console implements the kitty graphics protocol. */
TermGfxMode term_gfx_detect(void) { return TERM_GFX_NONE; }
int term_gfx_active(void) { return 0; }
void term_gfx_upload(const CoverData *cd) { (void)cd; }
void term_gfx_place(int cols, int rows) { (void)cols; (void)rows; }
void term_gfx_clear(void) {}
void term_gfx_upload_id(uint64_t id, const CoverData *cd) { (void)id; (void)cd; }
void term_gfx_place_id(uint64_t id, int row0, int col0, int cols, int rows) { (void)id; (void)row0; (void)col0; (void)cols; (void)rows; }
void term_gfx_delete_id(uint64_t id) { (void)id; }
void term_gfx_clear_ids(void) {}
int term_gfx_id_placed(uint64_t id) { (void)id; return 0; }
#else

/* ── Protocol state ─────────────────────────────────── */
static TermGfxMode g_mode = TERM_GFX_NONE;
static int g_detected = 0;

/* fingerprint of the image currently uploaded — the cover stamp, not the
   pixel pointer: the old buffer is freed on cover change and malloc may
   reuse the same address for same-dimension covers, which a pointer
   compare cannot tell apart (stale cover would stay on screen) */
static uint64_t g_uploaded_stamp = 0;
static int g_uploaded_w = 0;
static int g_uploaded_h = 0;

/* stable id handed out to the terminal (kitty i= parameter) */
static unsigned long g_img_id = 0;

/* fixed placement id (kitty p= parameter): re-placing the same
   (image id, placement id) pair atomically replaces the previous
   placement instead of stacking a new one on top */
#define TERM_GFX_PLACEMENT_ID 1

/* ── Multi-image bookkeeping ─────────────────────────────
   Song-list covers each live under an explicit caller-assigned id. We
   track, per id, the uploaded fingerprint (to skip re-transfers) and the
   last placement (to detect moves cheaply). A small fixed table is
   enough: only the visible rows' covers are alive at once. */
#define TERM_GFX_MAX_IMGS 64

typedef struct {
    uint64_t id;          /* 0 = free slot */
    uint64_t stamp;       /* uploaded CoverData.stamp */
    int      w, h;        /* uploaded dimensions      */
    int      placed;      /* 1 = a placement is currently live */
    int      p_row, p_col, p_cols, p_rows;  /* last placement rect */
} TermGfxImg;

static TermGfxImg g_imgs[TERM_GFX_MAX_IMGS];

static TermGfxImg *img_find(uint64_t id) {
    for (int i = 0; i < TERM_GFX_MAX_IMGS; i++)
        if (g_imgs[i].id == id) return &g_imgs[i];
    return NULL;
}

static TermGfxImg *img_slot(void) {
    for (int i = 0; i < TERM_GFX_MAX_IMGS; i++)
        if (g_imgs[i].id == 0) return &g_imgs[i];
    return NULL;
}

/* ── Raw escape output ───────────────────────────────── */
static void esc_write(const char *s) { fwrite(s, 1, strlen(s), stdout); }

static void kitty_delete_img(uint64_t id) {
    char seq[96];
    snprintf(seq, sizeof(seq), "\x1b_Ga=d,d=i,i=%lu\x1b\\", id);
    esc_write(seq);
}

/* ── Base64 (RFC 4648) ──────────────────────────────── */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_size(size_t in) { return ((in + 2) / 3) * 4; }

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

/* ── Detection ───────────────────────────────────────── */
TermGfxMode term_gfx_detect(void) {
    if (g_detected) return g_mode;
    g_detected = 1;

    const char *kitty_env = getenv("KITTY_WINDOW_ID");
    const char *term = getenv("TERM");
    /* Detection is TERM-driven. KITTY_WINDOW_ID alone is NOT enough: it
       leaks into unrelated terminals when exported from a kitty session
       (stale shell rc / environment.d), and treating a plain terminal
       (e.g. GNOME Terminal) as kitty leaves image-based UI — like the
       QR login screen — permanently blank. wezterm/foot/ghostty set
       TERM to themselves (or xterm-kitty); verify the protocol is
       actually claimed by TERM first. */
    if (term && (strstr(term, "kitty") || strstr(term, "foot") ||
                 strstr(term, "wezterm") || strstr(term, "ghostty"))) {
        g_mode = TERM_GFX_KITTY;
    } else if (kitty_env && kitty_env[0] &&
               term && strstr(term, "kitty")) {
        /* corroborated case: env var + TERM both claim kitty */
        g_mode = TERM_GFX_KITTY;
    }
    if (g_mode != TERM_GFX_NONE)
        LOG_INFO("Terminal graphics: kitty protocol");
    return g_mode;
}

int term_gfx_active(void) { return term_gfx_detect() == TERM_GFX_KITTY; }

/* ── Upload (chunked a=T transfer, q=2 silent) ───────── */
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
            /* q=2: no response from the terminal — an a=T reply would
               otherwise land in stdin and confuse the UI event loop */
            snprintf(header, sizeof(header),
                     "\x1b_Ga=T,i=%lu,q=2,f=24,s=%d,v=%d,m=%d;",
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

void term_gfx_upload(const CoverData *cd) {
    if (!term_gfx_active() || !cd || !cd->pixels) return;

    /* skip re-transfer when nothing changed */
    if (cd->stamp != 0 && cd->stamp == g_uploaded_stamp &&
        g_uploaded_w == cd->width && g_uploaded_h == cd->height)
        return;

    /* replacing the cover: drop the previous image's placements AND data
       (uppercase d=I frees the data) so old artwork doesn't linger under
       the new placement or eat the terminal's image quota */
    if (g_img_id != 0) {
        char seq[96];
        snprintf(seq, sizeof(seq), "\x1b_Ga=d,d=I,i=%lu\x1b\\", g_img_id);
        esc_write(seq);
    }

    g_img_id++;
    if (g_img_id == 0) g_img_id = 1;  /* id 0 is invalid */

    /* cover.c stores RGB24 (3 channels) */
    kitty_upload_raw(cd->pixels, cd->width, cd->height, g_img_id);
    g_uploaded_stamp = cd->stamp;
    g_uploaded_w = cd->width;
    g_uploaded_h = cd->height;
    LOG_DEBUG("term_gfx: uploaded cover %dx%d id=%lu stamp=%llu",
              cd->width, cd->height, g_img_id,
              (unsigned long long)cd->stamp);
}

void term_gfx_place(int cols, int rows) {
    if (!term_gfx_active() || g_img_id == 0 || cols <= 0 || rows <= 0)
        return;
    char seq[160];
    /* Drop stale placements of this image first (lowercase d=i keeps the
       uploaded data). Without this every a=p adds an additional
       placement, and old ones linger after terminal resizes — the ghost
       covers that corrupt the layout. Both escapes go out in one flush,
       so the terminal applies them before the next frame: no flicker. */
    snprintf(seq, sizeof(seq), "\x1b_Ga=d,d=i,i=%lu\x1b\\", g_img_id);
    esc_write(seq);
    /* a=p place, i=id, p= fixed placement id (re-placing the same
       (image id, placement id) pair atomically replaces the previous
       placement on terminals that support placement ids), q=2 no
       response. Both c and r are given so the image fills exactly the
       reserved placeholder rectangle (dw×dh from cover_layout) — with c
       alone kitty scales by aspect and can overflow the placeholder when
       the height cap kicked in. C=1 forbids kitty from moving the cursor
       after placement — otherwise the next FTXUI diff output would be
       written from the wrong cursor position. */
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=p,i=%lu,p=%d,q=2,c=%d,r=%d,C=1\x1b\\",
             g_img_id, TERM_GFX_PLACEMENT_ID, cols, rows);
    esc_write(seq);
}

void term_gfx_clear(void) {
    if (!term_gfx_active()) return;
    /* a=d delete, d=A deletes ALL images (and frees their data) */
    esc_write("\x1b_Ga=d,d=A\x1b\\");
    g_uploaded_stamp = 0;
    g_uploaded_w = g_uploaded_h = 0;
    /* d=A also removed every multi-image placement AND data. Reset the
       table so the next update_list_covers() pass re-uploads + re-places
       instead of skipping via its stale "already placed / already
       uploaded" bookkeeping. */
    for (int i = 0; i < TERM_GFX_MAX_IMGS; i++) {
        g_imgs[i].stamp = 0;
        g_imgs[i].w = g_imgs[i].h = 0;
        g_imgs[i].placed = 0;
    }
}

/* ── Multi-image management ─────────────────────────── */

/* Upload the cover pixels under an explicit caller id. The upload uses
   the caller's id directly (no g_img_id indirection), so it coexists
   with the single-image cover/QR paths. When the same id is re-uploaded
   with an unchanged fingerprint it is a no-op. */
void term_gfx_upload_id(uint64_t id, const CoverData *cd) {
    if (!term_gfx_active() || id == 0 || !cd || !cd->pixels) return;
    TermGfxImg *img = img_find(id);
    if (!img) {
        img = img_slot();
        if (!img) return;   /* table full: skip (visible rows are limited) */
        img->id = id;
    }
    if (img->stamp == cd->stamp && img->w == cd->width &&
        img->h == cd->height)
        return;  /* unchanged: keep uploaded data */

    /* replacing a different cover under this id: drop the old data +
       placements first */
    if (img->stamp != 0 || img->w != 0)
        kitty_delete_img(id);

    kitty_upload_raw(cd->pixels, cd->width, cd->height, id);
    img->stamp = cd->stamp;
    img->w = cd->width;
    img->h = cd->height;
    img->placed = 0;
    LOG_DEBUG("term_gfx: uploaded id=%lu %dx%d", id, cd->width, cd->height);
}

/* Place the id image at an absolute position. Replacing the previous
   placement of the same id (same image+placement id pair) atomically
   moves it; stale placements are dropped first so a move never leaves a
   ghost. */
void term_gfx_place_id(uint64_t id, int row0, int col0, int cols, int rows) {
    if (!term_gfx_active() || id == 0 || cols <= 0 || rows <= 0) return;
    TermGfxImg *img = img_find(id);
    if (!img || img->stamp == 0) return;   /* not uploaded yet */
    if (img->placed && img->p_row == row0 && img->p_col == col0 &&
        img->p_cols == cols && img->p_rows == rows)
        return;  /* already there */

    kitty_delete_img(id);
    char seq[160];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=p,i=%lu,p=%d,q=2,c=%d,r=%d,C=1\x1b\\",
             id, TERM_GFX_PLACEMENT_ID, cols, rows);
    /* Move the cursor to the top-left cell of the image first */
    printf("\x1b[%d;%dH", row0, col0);
    fflush(stdout);
    esc_write(seq);
    img->placed = 1;
    img->p_row = row0; img->p_col = col0;
    img->p_cols = cols; img->p_rows = rows;
}

/* Delete an id's image: free its data + placements and reset its slot. */
void term_gfx_delete_id(uint64_t id) {
    if (!term_gfx_active() || id == 0) return;
    TermGfxImg *img = img_find(id);
    if (!img) return;
    kitty_delete_img(id);
    img->id = 0;
    img->stamp = 0;
    img->w = img->h = 0;
    img->placed = 0;
}

/* Delete every multi-image slot. The single-image cover/QR data (which
   uses a separate id space) is untouched. */
void term_gfx_clear_ids(void) {
    if (!term_gfx_active()) return;
    for (int i = 0; i < TERM_GFX_MAX_IMGS; i++) {
        if (g_imgs[i].id != 0) {
            kitty_delete_img(g_imgs[i].id);
            g_imgs[i].id = 0;
            g_imgs[i].stamp = 0;
            g_imgs[i].w = g_imgs[i].h = 0;
            g_imgs[i].placed = 0;
        }
    }
}

int term_gfx_id_placed(uint64_t id) {
    TermGfxImg *img = img_find(id);
    return img ? img->placed : 0;
}

#endif /* _WIN32 */
