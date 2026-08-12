#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/keybindings.h"
#include "ui/state_store.h"

/* Render the full-page help screen showing all available keybindings.
   Key names are resolved from the live KeybindingManager so user
   custom bindings are reflected. */
ftxui::Element render_help_screen(const AppState &state,
                                  const KeybindingManager &kb);
