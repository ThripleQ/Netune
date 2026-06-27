#include "ui/components/search_bar.h"
#include "ui/components/theme_util.h"
#include <cstdio>
using namespace ftxui;

Element render_search_bar(const AppState &s) {
    if (!s.search_active) return text("");

    /* Build results list */
    Elements result_els;
    if (s.search_query.empty()) {
        result_els.push_back(theme_fg(text(" Type to search... ")) | dim);
    } else if (s.music_mode == MusicMode::Netease && s.search_results.empty()) {
        result_els.push_back(theme_fg(text(" Press [Enter] to search Netease ")) | dim);
    } else if (s.search_results.empty()) {
        result_els.push_back(theme_fg(text(" No results. ")) | dim);
    } else {
        char header[64];
        snprintf(header, sizeof(header), " %d/%d results:",
                 (int)s.search_results.size(), s.search_total);
        result_els.push_back(theme_accent(text(header) | bold));
        result_els.push_back(separator());

        int shown = 0;
        for (auto &song : s.search_results) {
            if (shown >= 20) break;
            bool selected = (shown == s.search_selected);
            std::string label;
            if (song.title) label += song.title;
            if (song.artist) { label += "  —  "; label += song.artist; }
            if (selected)
                result_els.push_back(theme_accent(text("> " + label) | bold | inverted));
            else
                result_els.push_back(theme_fg(text("  " + label)));
            shown++;
        }
    }

    /* Build search overlay */
    Elements col;
    col.push_back(text(" Search ") | bold | center | underlined);
    col.push_back(separator());
    col.push_back(theme_accent(text(" / " + s.search_query + "▌")));
    col.push_back(separator());
    col.push_back(vbox(std::move(result_els)) | yframe | flex);
    col.push_back(separator());
    std::string hint;
    if (s.music_mode == MusicMode::Netease)
        hint = s.search_results.empty() ?
            " [Enter]search  [Esc]close" :
            " [↑↓]select  [Enter]play  [Esc]close";
    else
        hint = " [↑↓]select  [Enter]open  [Esc]close";
    col.push_back(theme_fg(text(hint)) | dim | center);

    auto box = vbox(std::move(col));
    return box | border | center | clear_under | bgcolor(Color::RGB(15, 15, 25));
}
