#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/state_store.h"

/* Render the Ctrl+X action sheet overlay: like/unlike a song or
   subscribe/unsubscribe a playlist from the currently selected
   right-panel item. */
ftxui::Element render_action_sheet(const AppState &state);

/* Render the song-detail popup (key d). */
ftxui::Element render_song_detail(const AppState &state);
