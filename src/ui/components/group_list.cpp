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
        if (s.active_panel == 1) {
            /* inside a group: songs on the right, show a back hint
               (Esc returns to the group list). */
            int gi = s.group_index;
            if (gi >= 0 && gi < (int)s.groups.size()) {
                els.push_back(theme_fg(text("  << \u8FD4\u56DE \u672C\u5730 ") | bold));  /* << 返回 本地 */
                els.push_back(theme_selection(
                    hflow(paragraph("> " + s.groups[gi].name)) | bold | focus));
            } else {
                els.push_back(theme_fg(text("  (\u7A7A)")) | dim);  /* (空) */
            }
        } else {
        /* local groups. The netease entry is a navigation item —
           hidden while the top search box is being edited. */
        bool any = false;

        if (!s.top_search_active) {
            bool netease_sel = (s.active_panel == 0 && s.group_index < 0);
            if (netease_sel)
                els.push_back(theme_selection(text("> >> 网易云音乐") | bold | focus));
            else
                els.push_back(theme_fg(text("  >> 网易云音乐")) | dim);
        }

        for (size_t i = 0; i < s.groups.size(); i++) {
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
        if (!any)
            els.push_back(theme_fg(text("  (空)")) | dim);
        }
    } else {
        /* netease mode: menu items. The back-to-local entry is
           navigation — hidden while searching, rendered at the
           bottom of the list. */
        bool any = false;
        for (size_t i = 0; i < s.netease_menu.size(); i++) {
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
        if (!any)
            els.push_back(theme_fg(text("  (空)")) | dim);

        /* the local navigation entry stays visible while the top search
           box is being edited — only its highlight is suppressed */
        bool back_sel = (s.active_panel == 0 && s.netease_selected < 0 &&
                         !s.top_search_active);
        if (back_sel)
            els.push_back(theme_selection(text("> 本地") | bold | focus));
        else
            els.push_back(theme_fg(text("  本地")) | dim);
    }

    return theme_border(theme_bg(vbox(std::move(els)) | yframe | size(WIDTH, EQUAL, 18) | border));
}
