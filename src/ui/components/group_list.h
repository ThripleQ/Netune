#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the left panel: folder group list.
   active_panel 0 → show selection marker. */
ftxui::Element render_group_list(const AppState &state);
