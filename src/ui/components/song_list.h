#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the right panel: song list for current group.
   active_panel 1 → show selection marker. */
ftxui::Element render_song_list(const AppState &state);

/* Width (columns) of the square cover placeholder used by cover-art list
   rows (image terminals only). The frame hook in app.cpp uses the same
   value to place the overlaid image exactly over the placeholder. */
int song_list_cover_cols(void);
