#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the top title bar: app name + version + playing song.
   Replaces the hardcoded " Netune v2.0.0 " text. */
ftxui::Element render_top_bar(const AppState &state);
