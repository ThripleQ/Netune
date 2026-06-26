#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the top status line: play state + loop mode + time + volume + title.
   Also renders the progress gauge. */
ftxui::Element render_status_bar(const AppState &state);
