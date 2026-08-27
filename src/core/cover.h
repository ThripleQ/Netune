#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Raw cover image data ──────────────────────── */
typedef struct {
    uint8_t *pixels;  /* RGBA or RGB, row-major */
    int      width;
    int      height;
    int      channels; /* 3=RGB, 4=RGBA */
    /* Monotonic id assigned by cover_load(); uniquely identifies the image
       content. Used as the upload fingerprint by term_gfx — comparing the
       pixel pointer alone is unsafe (the old buffer is freed and malloc
       may hand out the same address for the next cover, which would skip
       the re-upload and keep showing the previous cover). */
    uint64_t stamp;
} CoverData;

/* ── API ──────────────────────────────────────── */

/* Download cover from URL into raw pixels.
   Uses libcurl internally.
   Returns 0 on success, -1 on failure. Caller owns *out. */
int cover_load(const char *url, CoverData *out);

/* Download cover from URL, downscaled to fit the max_w x max_h box
   (aspect preserved, never upscaled). The song-list cover overlay only
   needs a ~32x32px image; decoding+uploading the full-resolution cover
   (up to 2048px) for every visible row turned a ~1MB base64 transfer on
   the main thread into the list's main performance bottleneck. max_w or
   max_h <= 0 means "no box constraint" (identical to cover_load). */
int cover_load_to(const char *url, int max_w, int max_h, CoverData *out);

/* Free cover pixel data. */
void cover_free(CoverData *cd);

/* Allocate the next unique CoverData.stamp. Thread-safe; used by code
   that reconstructs CoverData outside cover_load (e.g. reading a cover
   back from the disk cache) so the upload fingerprint stays unique. */
uint64_t cover_stamp_next(void);

/* ── Terminal cell metrics ──────────────────────────
   Probed once at startup via CSI queries (14t/18t) so the character
   renderer can sample cover pixels at the true cell aspect ratio
   (cells are often not 2:1, e.g. Adwaita Mono). Falls back to 2:1. */
void cover_cell_probe(void);
int  cover_cell_width(void);   /* px, default 8  */
int  cover_cell_height(void);  /* px, default 16 */

#ifdef __cplusplus
}
#endif
