#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the bottom control hint bar.
   Dynamically shows relevant shortcuts based on current state. */
ftxui::Element render_player_controls(const AppState &state);
