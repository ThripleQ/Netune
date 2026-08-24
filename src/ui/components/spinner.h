#pragma once
#include "ui/state_store.h"
#include <string>
#include <ftxui/dom/elements.hpp>

ftxui::Element render_spinner(const AppState &s);

/* One animated spinner frame (wall-clock driven, ~12.5 fps). Handy for
   per-row suffixes where a full render_spinner element doesn't fit. */
std::string spinner_glyph(void);
