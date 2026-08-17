#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the Netease QR login overlay.
   Shows QR code, status messages, and polling indicator. */
ftxui::Element render_login_screen(const AppState &state);

/* Minimum terminal size (cols x rows) that can render the character QR
   without clipping modules. Returns false when qr_text is empty. Shared
   by the renderer and the polling loop (below this size polling is
   paused so the "network error" banner is not triggered by a small
   window). */
bool qr_min_dims(const std::string &qr_text, int *cols, int *rows);
