#include "ui/components/top_bar.h"
#include "ui/components/theme_util.h"
using namespace ftxui;

Element render_top_bar(const AppState &) {
    /* Show app name and mode, no song title — moved to status bar */
    return theme_bg(theme_fg(text(" ") | bold) | center);
}
