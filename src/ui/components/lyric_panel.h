#pragma once

#include <ftxui/dom/elements.hpp>

struct AppState;  /* forward decl from state_store.h */

/* Render the full-screen lyrics view.
   Layout: left column (album art placeholder) + right column (lyrics).
   Falls back to "No lyrics" if none loaded. */
ftxui::Element render_lyric_panel(const AppState &s);
