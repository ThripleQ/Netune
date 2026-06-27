#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render a help overlay showing all available keybindings.
   Caller wraps it on top of the main content. */
ftxui::Element render_help_screen(const AppState &state);
