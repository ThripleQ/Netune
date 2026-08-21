#include "ui/components/group_list.h"
#include "ui/components/theme_util.h"
#include "ui/ui_util.h"
#include <ftxui/screen/string.hpp>
#include <cstdio>
#include <string>
#include <cwchar>
#include <algorithm>
using namespace ftxui;

/* ── Render ──────────────────────────────────────────── */
Element render_group_list(const AppState &s) {
    Elements els;

    if (s.music_mode == MusicMode::Local) {
        /* local groups, filtered by the top-left search box.
           The netease entry is a navigation item — hidden while searching. */
        const std::string &q = s.top_left_query;
        bool any = false;

        if (q.empty()) {
            bool netease_sel = (s.active_panel == 0 && s.group_index < 0 && !s.top_search_active);
            if (netease_sel)
                els.push_back(theme_selection(text("> >> 网易云音乐") | bold | focus));
            else
                els.push_back(theme_fg(text("  >> 网易云音乐")) | dim);
        }

        for (size_t i = 0; i < s.groups.size(); i++) {
            if (!q.empty() && !str_icontains(s.groups[i].name, q))
                continue;
            any = true;
            std::string label = s.groups[i].name;
            bool sel = ((int)i == s.group_index);

            if (s.active_panel == 0 && sel && !s.top_search_active) {
                els.push_back(theme_selection(
                    hflow(paragraph("> " + label)) | bold | focus));
            } else if (s.active_panel == 1 && sel) {
                els.push_back(theme_fg(text("  " + label) | bold));
            } else {
                els.push_back(theme_fg(text("  " + label)));
            }
        }
        if (!any && !q.empty())
            els.push_back(theme_fg(text("  无匹配")) | dim);
    } else {
        /* netease mode: menu items, filtered by the top-left search box.
           The back-to-local entry is navigation — hidden while searching. */
        const std::string &q = s.top_left_query;

        if (q.empty()) {
            bool back_sel = (s.active_panel == 0 && s.netease_selected < 0 && !s.top_search_active);
            if (back_sel)
                els.push_back(theme_selection(text("> << 本地音乐") | bold | focus));
            else
                els.push_back(theme_fg(text("  << 本地音乐")) | dim);
            els.push_back(separator());
        }

        bool any = false;
        for (size_t i = 0; i < s.netease_menu.size(); i++) {
            if (!q.empty() && !str_icontains(s.netease_menu[i].name, q))
                continue;
            any = true;
            std::string label = s.netease_menu[i].name;
            bool sel = ((int)i == s.netease_selected);

            if (s.active_panel == 0 && sel && !s.top_search_active) {
                els.push_back(theme_selection(
                    hflow(paragraph("> " + label)) | bold | focus));
            } else {
                els.push_back(theme_fg(text("  " + label)));
            }
        }
        if (!any && !q.empty())
            els.push_back(theme_fg(text("  无匹配")) | dim);
    }

    return theme_bg(vbox(std::move(els)) | yframe | size(WIDTH, EQUAL, 18) | border);
}
