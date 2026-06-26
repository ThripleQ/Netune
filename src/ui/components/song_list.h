#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the right panel: song list for current group.
   active_panel 1 → show selection marker. */
ftxui::Element render_song_list(const AppState &state);
