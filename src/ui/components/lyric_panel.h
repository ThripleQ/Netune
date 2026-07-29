#pragma once

#include <ftxui/dom/elements.hpp>

struct AppState;

/* Cover panel only (left side) */
ftxui::Element render_cover_only(const AppState &s);

/* Lyrics panel only (right side) */
ftxui::Element render_lyrics_only(const AppState &s);

/* Combined cover + lyrics (for simple layout) */
ftxui::Element render_lyric_panel(const AppState &s);

/* Spectrum bar (2 rows, 16-level bars, full width) */
ftxui::Element render_spectrum_bar(const AppState &s);
