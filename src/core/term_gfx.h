#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "core/cover.h"

/* ── Terminal graphics capability ───────────────────
   Some terminals (kitty, wezterm, foot, ghostty, konsole...) can render
   raw image data natively via the kitty graphics protocol. When available
   the cover is drawn as a real image placed over the UI after each frame;
   otherwise the half-block character renderer is used.

   Placement strategy: upload once per cover (fingerprinted), then
   re-place periodically (~0.5s) from the caller — terminal resizes,
   fullscreen toggles and window switches can drop or relocate placed
   images, and a periodic re-place heals that within half a second with
   no fragile resize detection. */

typedef enum {
    TERM_GFX_NONE,   /* fall back to half-block rendering */
    TERM_GFX_KITTY,  /* kitty graphics protocol            */
} TermGfxMode;

TermGfxMode term_gfx_detect(void);

/* True when a native-image path is available. */
int term_gfx_active(void);

/* Upload the cover pixels to the terminal under a stable id. No-op when
   the image didn't change since the last call. Safe every frame. */
void term_gfx_upload(const CoverData *cd);

/* Place the uploaded image at the current cursor position, spanning
   `cols` terminal columns (kitty scales keeping the aspect ratio).
   C=1 keeps the cursor in place. */
void term_gfx_place(int cols);

/* Delete all placed images (e.g. when leaving lyric mode). */
void term_gfx_clear(void);

#ifdef __cplusplus
}
#endif
