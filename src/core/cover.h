#pragma once

#include <stddef.h>
#include <stdint.h>

/* ── Raw cover image data ──────────────────────── */
typedef struct {
    uint8_t *pixels;  /* RGBA or RGB, row-major */
    int      width;
    int      height;
    int      channels; /* 3=RGB, 4=RGBA */
} CoverData;

/* ── API ──────────────────────────────────────── */

/* Download cover from URL into raw pixels.
   Uses libcurl internally. Caches result in ~/.cache/netune/covers/.
   Returns 0 on success, -1 on failure. Caller owns *out. */
int cover_load(const char *url, CoverData *out);

/* Free cover pixel data. */
void cover_free(CoverData *cd);

/* ── Terminal cell metrics ──────────────────────────
   Probed once at startup via CSI queries (14t/18t) so the character
   renderer can sample cover pixels at the true cell aspect ratio
   (cells are often not 2:1, e.g. Adwaita Mono). Falls back to 2:1. */
void cover_cell_probe(void);
int  cover_cell_width(void);   /* px, default 8  */
int  cover_cell_height(void);  /* px, default 16 */
