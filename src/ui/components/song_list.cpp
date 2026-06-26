#include "ui/components/song_list.h"
#include "ui/components/theme_util.h"
using namespace ftxui;

Element render_song_list(const AppState &s) {
    Elements els;
    for (size_t i = 0; i < s.playlist.size(); i++) {
        std::string label = s.playlist[i].title
            ? s.playlist[i].title : "(unknown)";
        bool sel = ((int)i == s.selected_index);
        if (s.active_panel == 1 && sel)
            els.push_back(theme_accent(text("> " + label) | bold | inverted));
        else if (s.active_panel == 0 && sel)
            els.push_back(theme_fg(text("  " + label)));
        else
            els.push_back(theme_fg(text("  " + label)));
    }
    return theme_bg(vbox(std::move(els)) | yframe | flex | border);
}
