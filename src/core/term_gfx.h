#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "core/cover.h"

/* ── Terminal graphics capability ───────────────────
   Some terminals (kitty, wezterm, foot, ghostty, konsole...) can render
   raw image data natively via the kitty graphics protocol. When available,
   the cover is drawn as a real image overlaid after each frame instead of
   half-block characters. Detected once at startup; GNOME Terminal and
   friends fall back to the character renderer. */

typedef enum {
    TERM_GFX_NONE,   /* fall back to half-block rendering */
    TERM_GFX_KITTY,  /* kitty graphics protocol            */
} TermGfxMode;

TermGfxMode term_gfx_detect(void);

/* True when a native-image path is available. */
int term_gfx_active(void);

/* Upload the cover pixels to the terminal (kitty a=T, cached under a
   stable id). No-op unless the image actually changed since the last
   call. Safe to call every frame. */
void term_gfx_upload(const CoverData *cd);

/* Place the uploaded image at the current cursor position, spanning
   `cols` terminal columns and `rows` terminal rows (cell-aligned,
   aspect ratio preserved). */
void term_gfx_place(int cols, int rows);

/* Delete all placed images (e.g. when leaving lyric mode). */
void term_gfx_clear(void);

#ifdef __cplusplus
}
#endif
