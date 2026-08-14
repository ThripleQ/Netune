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

   Placement strategy: upload once per cover (fingerprinted by the cover
   stamp), then re-place from the caller — immediately whenever the
   layout geometry changes (terminal resize, new cover) and periodically
   (~0.5s) otherwise. Terminal resizes, fullscreen toggles and window
   switches can drop or relocate placed images; the periodic re-place
   heals that with no fragile resize detection.

   Every placement uses a stable placement id (p=1) and stale placements
   of the image are deleted first, so re-placing REPLACES the previous
   image instead of adding another one on top — repeated a=p without a
   placement id creates a new placement each time, which leaves ghost
   covers at old positions after a resize. */

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
   `cols` terminal columns and `rows` rows (the image is scaled to fill
   exactly that rectangle — pass the dw×dh computed by cover_layout so it
   matches the reserved placeholder). Stale placements of the image are
   replaced, not stacked. C=1 keeps the cursor in place. */
void term_gfx_place(int cols, int rows);

/* Delete all placed images (e.g. when leaving lyric mode). */
void term_gfx_clear(void);

#ifdef __cplusplus
}
#endif
