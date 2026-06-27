#include "ui/components/top_bar.h"
#include "ui/components/theme_util.h"
using namespace ftxui;

Element render_top_bar(const AppState &s) {
    std::string title;
    if (s.current_song.title && s.current_song.title[0]) {
        title = s.current_song.title;
        if (s.current_song.artist && s.current_song.artist[0]) {
            title += std::string(" — ") + s.current_song.artist;
        }
    } else {
        title = "LMusic v2.0.0";
    }
    return theme_bg(theme_fg(text(" " + title) | bold) | center);
}
