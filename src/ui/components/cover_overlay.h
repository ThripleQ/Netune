#pragma once

#include <ftxui/dom/elements.hpp>
#include <cstdint>
#include <vector>

/* ── Song-list cover overlay (image terminals) ──────────
   The cover placeholder on each list row is a CoverNode: a custom FTXUI
   node whose SetBox() records the FINAL screen box (already scroll-offset
   by the parent yframe) that FTXUI assigns to that row's cover column.
   The main loop's update_list_covers() then reads those boxes — the
   layout's ground truth — and places the real image exactly there, with
   zero manual coordinate arithmetic.

   The slot table is keyed by song index and PERSISTS across frames:
   SetBox() overwrites the slot each time it runs, so a frame where FTXUI
   skips re-layout (no visible change) keeps the previous frame's boxes
   and the overlay does not flicker by deleting/re-placing everything.
   Only visible rows (box within the viewport) are placed; rows scrolled
   out get a box outside the screen and are skipped/deleted. */

namespace cover_overlay {

/* One recorded slot: the box FTXUI assigned to a given song row's cover
   column. `id` is the cover_cache id (0 = no cover to show). */
struct Slot {
    int        song_idx = -1;
    uint64_t   id = 0;
    int        x = 0, y = 0;   /* box x_min / y_min (0-based) */
    int        w = 0, h = 0;   /* box width / height in cells */
    uint64_t   gen = 0;        /* SetBox generation when last laid out */
};

/* The slot table. Slots persist across frames; SetBox overwrites the
   entry for its row each layout pass. Fixed capacity: covers the visible
   window. */
std::vector<Slot>& slots();

/* Monotonic layout-generation counter. Every CoverNode::SetBox() stamps
   its slot with the current generation, so update_list_covers() can tell
   which slots were laid out THIS frame (rendered) vs. those left over
   from earlier frames (scrolled off / removed — their images must be
   deleted). */
uint64_t gen();

/* Create the placeholder element for a song row's cover column.
   `url` may be empty (no cover); the node then records an empty slot. */
ftxui::Element coverPlaceholder(int song_idx, const char* url);

/* Width (columns) of the square cover placeholder. */
int cols();

/* Rows the image spans (2: title row + artist row). */
int rows();

}  // namespace cover_overlay
