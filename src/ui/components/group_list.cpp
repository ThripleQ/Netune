#include "ui/components/group_list.h"
#include "ui/components/theme_util.h"
#include <cstdio>
using namespace ftxui;

Element render_group_list(const AppState &s) {
    Elements els;

    if (s.music_mode == MusicMode::Local) {
        /* ── Local mode: show "网易云音乐" entry + folders ── */
        bool netease_sel = (s.active_panel == 0 && s.group_index < 0);
        if (netease_sel)
            els.push_back(theme_accent(text("> >> 网易云音乐") | bold | inverted));
        else
            els.push_back(theme_fg(text("  >> 网易云音乐") | dim));

        for (size_t i = 0; i < s.groups.size(); i++) {
            std::string label = s.groups[i].name;
            bool sel = ((int)i == s.group_index);
            if (s.active_panel == 0 && sel)
                els.push_back(theme_accent(text("> " + label) | bold | inverted));
            else if (s.active_panel == 1 && sel)
                els.push_back(theme_fg(text("  " + label) | bold));
            else
                els.push_back(theme_fg(text("  " + label)));
        }
    } else {
        /* ── Netease mode: show "<< 本地音乐" back + netease menu ── */
        bool back_sel = (s.active_panel == 0 && s.netease_selected < 0);
        if (back_sel)
            els.push_back(theme_accent(text("> << 本地音乐") | bold | inverted));
        else
            els.push_back(theme_fg(text("  << 本地音乐") | dim));

        els.push_back(separator());

        for (size_t i = 0; i < s.netease_menu.size(); i++) {
            std::string label = s.netease_menu[i].name;
            bool sel = ((int)i == s.netease_selected);
            if (s.active_panel == 0 && sel)
                els.push_back(theme_accent(text("> " + label)) | bold);
            else
                els.push_back(theme_fg(text("  " + label)));
        }
    }

    return theme_bg(vbox(std::move(els)) | yframe | size(WIDTH, EQUAL, 22) | border);
}
