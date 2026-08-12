#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "core/cover.h"

/* ── Terminal graphics capability ───────────────────
   Some terminals (kitty, wezterm, foot, ghostty, konsole...) can render
   raw image data natively via the kitty graphics protocol. When available,
   the cover is drawn as a real image using the Unicode placeholder
   mechanism (U+10EEEE): the image ID is encoded in the cell's foreground
   color, and the cell grid itself is normal text — so FTXUI moves, diffs
   and removes it exactly like any other text. No cursor manipulation,
   no placement tracking, no explicit cleanup needed. */

typedef enum {
    TERM_GFX_NONE,   /* fall back to half-block rendering */
    TERM_GFX_KITTY,  /* kitty graphics protocol            */
} TermGfxMode;

TermGfxMode term_gfx_detect(void);

/* True when a native-image path is available. */
int term_gfx_active(void);

/* Upload the cover pixels to the terminal (a=T) and create/recreate the
   virtual placement matching the rendered grid (a=p,U=1). Both are
   no-ops unless the image or grid changed. Safe to call every frame. */
void term_gfx_ensure(const CoverData *cd, int cols, int rows);

/* 24-bit image id currently uploaded (0 = none). Encoded into the
   placeholder cells' foreground color by the renderer. */
unsigned long term_gfx_image_id(void);

#ifdef __cplusplus
}
#endif
