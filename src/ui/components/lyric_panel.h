#pragma once

#include <ftxui/dom/elements.hpp>

struct AppState;

/* Cover panel only (left side) */
ftxui::Element render_cover_only(const AppState &s);

/* Cover cell layout shared by the character renderer and the raw-image
   overlay: cw = panel width in columns (clamped 12-60), dh = rendered
   rows for the current cover (capped at the lyrics panel height 20). */
void cover_layout(const AppState &s, int *cw, int *dh);

/* Left margin before the cover: 1/16 of the terminal width. */
int cover_left_margin(const AppState &s);

/* Lyrics panel only (right side) */
ftxui::Element render_lyrics_only(const AppState &s);

/* Combined cover + lyrics (for simple layout) */
ftxui::Element render_lyric_panel(const AppState &s);

/* Spectrum bar (2 rows, 16-level bars, full width) */
ftxui::Element render_spectrum_bar(const AppState &s);
