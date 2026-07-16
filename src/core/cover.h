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
