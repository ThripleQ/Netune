#pragma once

#include <ftxui/dom/elements.hpp>

struct AppState;

/* Cover panel only (left side) */
ftxui::Element render_cover_only(const AppState &s);

/* Cover cell layout shared by the character renderer and the raw-image
   overlay:
   - cw = slot width in columns (60% of the panel, 12-column floor;
          lyrics layout follows it)
   - dw = actual cover width in columns (≤ cw, centered inside the slot;
          shrunk instead of distorting when the height cap kicks in)
   - dh = rendered rows for the current cover (capped at 2/5 of the
          terminal height, 12-row floor)
   Any output pointer may be NULL. */
void cover_layout(const AppState &s, int *cw, int *dw, int *dh);

/* Left margin before the cover: 1/16 of the terminal width. */
int cover_left_margin(const AppState &s);

/* Lyrics panel only (right side) */
ftxui::Element render_lyrics_only(const AppState &s);

/* Combined cover + lyrics (for simple layout) */
ftxui::Element render_lyric_panel(const AppState &s);

/* Spectrum bar (2 rows, 16-level bars, full width) */
ftxui::Element render_spectrum_bar(const AppState &s);
