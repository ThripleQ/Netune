#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the search interface: input bar + results overlay.
   When search_active is true, shows an overlay with the search query
   input line and matching results. */
ftxui::Element render_search_bar(const AppState &state);
