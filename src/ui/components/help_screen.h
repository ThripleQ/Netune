#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the full-page help screen showing all available keybindings. */
ftxui::Element render_help_screen(const AppState &state);
