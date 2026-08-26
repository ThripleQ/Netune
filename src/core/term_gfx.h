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

/* ── Multi-image management (song-list covers) ─────────
   The single-image API above (upload/place/clear) drives the lyric-mode
   cover and the QR code. The song list needs MANY covers alive at once,
   each tracked by an explicit image id assigned by the caller (a stable
   hash of the cover URL). Images are managed independently: uploading or
   placing one never touches the others, and deleting one frees only its
   own data + placements. All ids must be non-zero (0 is reserved). */

/* Upload cover pixels under an explicit image id. No-op if the id is
   already uploaded with identical (stamp, w, h). Safe to call every
   frame. */
void term_gfx_upload_id(uint64_t id, const CoverData *cd);

/* Place the id image at an ABSOLUTE terminal position (1-based row/col,
   top-left cell of the image) spanning cols×rows cells. Replaces any
   previous placement of the same id; other images are untouched. */
void term_gfx_place_id(uint64_t id, int row0, int col0, int cols, int rows);

/* Delete the id image: frees its data and removes its placements. */
void term_gfx_delete_id(uint64_t id);

/* Delete every image managed through the multi-image API (leaves the
   single-image cover/QR data alone). */
void term_gfx_clear_ids(void);

/* Last-seen placement of an id, to detect repositioning cheaply.
   Returns 0 if the id has no placement. */
int term_gfx_id_placed(uint64_t id);

#ifdef __cplusplus
}
#endif
