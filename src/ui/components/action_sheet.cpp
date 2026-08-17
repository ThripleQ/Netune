#include "ui/components/action_sheet.h"
#include "ui/components/theme_util.h"
#include <string>
using namespace ftxui;

Element render_action_sheet(const AppState &s) {
    if (!s.action_sheet_open) return text("");

    const auto &item = s.playlist.empty() ? SongInfo{} :
                       s.playlist[s.selected_index];
    bool is_playlist = item.aux_label && std::string(item.aux_label) == "歌单";
    std::string title = item.title ? item.title : "(nothing selected)";

    /* Option rows: like (song) or subscribe (playlist) */
    struct Opt { std::string label; };
    std::vector<Opt> opts;
    if (is_playlist) {
        opts.push_back({"\u2606  \u6536\u85CF\u6B4C\u5355"});   /* ☆ 收藏歌单 */
        opts.push_back({"\u2606  \u53D6\u6D88\u6536\u85CF"});   /* ☆ 取消收藏 */
    } else {
        opts.push_back({"\u2665  \u559C\u6B22\u6B64\u6B4C\u66F2"});     /* ♥ 喜欢此歌曲 */
        opts.push_back({"\u2661  \u53D6\u6D88\u559C\u6B22"});     /* ♡ 取消喜欢 */
    }

    Elements rows;
    for (size_t i = 0; i < opts.size(); i++) {
        auto label = text(opts[i].label);
        if ((int)i == s.action_sheet_selected)
            rows.push_back(theme_accent(hbox({text(" \u203A "), label | bold})));
        else
            rows.push_back(hbox({text("   "), label}));
    }

    auto &th = ThemeManager::instance().current();
    auto box = vbox({
        text(" " + title + " ") | bold | center,
        separator(),
        text("  j/k \u79FB\u52A8  Enter \u6267\u884C  Esc \u5173\u95ED ") | dim | center,
        filler(),
        vbox(std::move(rows)),
        filler(),
    }) | borderRounded
      | color(Color::RGB(th.accent.r, th.accent.g, th.accent.b));

    /* center overlay over a dimmed backdrop */
    return hbox({filler(), vbox({filler(), box, filler()}), filler()})
         | size(WIDTH, EQUAL, 40) | size(HEIGHT, EQUAL, 9);
}
