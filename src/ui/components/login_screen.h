#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the Netease QR login overlay.
   Shows QR code, status messages, and polling indicator. */
ftxui::Element render_login_screen(const AppState &state);
