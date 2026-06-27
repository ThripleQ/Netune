#include "ui/components/player_controls.h"
#include "ui/components/theme_util.h"
#include <cstdio>
using namespace ftxui;

Element render_player_controls(const AppState &s) {
    std::string hints;
    hints += " [Tab]panel";
    hints += "  [j/k]nav";
    hints += "  [Enter]play";
    hints += "  [Space]pause";
    hints += "  [+/-]vol";

    /* loop mode hint */
    const char *loop_label = "None";
    switch (s.loop_mode) {
    case LoopMode::Track:    loop_label = "Track"; break;
    case LoopMode::Playlist: loop_label = "List";  break;
    default: break;
    }
    char loop_buf[32];
    snprintf(loop_buf, sizeof(loop_buf), "  [l]loop:%s", loop_label);

    hints += loop_buf;
    hints += "  [s]stop";
    hints += "  [m]mute";
    hints += "  [?]help";
    hints += "  [q]quit";

    return theme_bg(theme_fg(text(hints) | dim));
}
