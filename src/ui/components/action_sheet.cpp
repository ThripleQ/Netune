#include "ui/components/action_sheet.h"
#include "ui/components/theme_util.h"
#include "ui/theme.h"
#include <string>
#include <vector>
using namespace ftxui;

/* ── Option rows for the main action menu (menu 0) ── */
struct ActionOpt { std::string label; int id; };

Element render_action_sheet(const AppState &s) {
    if (!s.action_sheet_open) return text("");

    const auto &item = s.playlist.empty() ? SongInfo{} :
                       s.playlist[s.selected_index];
    bool is_playlist = item.is_playlist;
    std::string title = item.title ? item.title : "(nothing selected)";
    auto &th = ThemeManager::instance().current();

    Elements body;

    if (s.action_sheet_menu == 0) {
        /* main menu — options depend on the selected object:
           playlist:  subscribe/unsubscribe (any) + rename/delete (own only)
           song:      like/unlike (any) + add-to-playlist (any) +
                     remove-from-current (own playlist detail only) */
        std::vector<ActionOpt> opts;
        if (is_playlist) {
            opts.push_back({s.action_sheet_active == 1
                ? " \u2606 \u53D6\u6D88\u6536\u85CF\u6B4C\u5355 "   /* ☆ 取消收藏歌单 */
                : " \u2606 \u6536\u85CF\u6B4C\u5355 ", 1});       /* ☆ 收藏歌单 */
            if (item.mine == 1) {
                opts.push_back({" \u270E \u91CD\u547D\u540D... ", 2});     /* ✎ 重命名... */
                opts.push_back({" \u2716 \u5220\u9664\u6B4C\u5355 ", 3});  /* ✖ 删除歌单 */
            }
        } else {
            opts.push_back({s.action_sheet_active == 1
                ? " \u2665 \u53D6\u6D88\u559C\u6B22 "   /* ♥ 取消喜欢 */
                : " \u2665 \u559C\u6B22\u6B64\u6B4C\u66F2 ", 1});  /* ♥ 喜欢此歌曲 */
            opts.push_back({" \u21D2 \u6536\u85CF\u5230\u6B4C\u5355... ", 4});  /* ⇒ 收藏到歌单... */
            if (s.detail_playlist_mine) {
                opts.push_back({" \u2716 \u4ECE\u5F53\u524D\u6B4C\u5355\u79FB\u9664 ", 5}); /* ✖ 从当前歌单移除 */
            }
        }
        for (size_t i = 0; i < opts.size(); i++) {
            auto row = text(opts[i].label);
            if ((int)i == s.action_sheet_selected)
                body.push_back(hbox({theme_selection(text(" \u203A ")), row | bold}) | focus);
            else
                body.push_back(hbox({text("   "), row}));
        }
    } else if (s.action_sheet_menu == 1) {
        /* playlist picker */
        body.push_back(text(" \u9009\u62E9\u6B4C\u5355: ") | bold);  /* 选择歌单: */
        if (s.action_sheet_pls.empty()) {
            body.push_back(text("  (\u52A0\u8F7D\u4E2D...)") | dim);  /* (加载中...) */
        } else {
            for (size_t i = 0; i < s.action_sheet_pls.size(); i++) {
                auto row = text(std::string("  ") +
                                (s.action_sheet_pls[i].title ? s.action_sheet_pls[i].title : ""));
                if ((int)i == s.action_sheet_selected)
                    body.push_back(hbox({theme_selection(text(" \u203A ")), row | bold}) | focus);
                else
                    body.push_back(hbox({text("   "), row}));
            }
        }
    } else if (s.action_sheet_menu == 2) {
        /* text input */
        std::string label = s.action_sheet_ctx == "rename"
            ? " \u91CD\u547D\u540D: "        /* 重命名: */
            : " \u65B0\u5EFA\u6B4C\u5355: "; /* 新建歌单: */
        body.push_back(text(label) | bold);
        body.push_back(text(" \u2502" + s.action_sheet_input + "\u258C\u2502 ") | bold
                       | bgcolor(Color::RGB(th.accent.r, th.accent.g, th.accent.b))
                       | color(Color::RGB(0, 0, 0)));
        body.push_back(text("  Enter \u786E\u8BA4  Esc \u53D6\u6D88 ") | dim);
    } else if (s.action_sheet_menu == 3) {
        /* confirm */
        body.push_back(text(" \u786E\u8BA4\u5220\u9664\u6B4C\u5355? ") | bold
                       | color(Color::RGB(th.error.r, th.error.g, th.error.b)));
        body.push_back(text("  Enter \u786E\u8BA4  Esc \u53D6\u6D88 ") | dim);
    }

    auto box = vbox({
        text(" " + title + " ") | bold,
        separator(),
        vbox(std::move(body)),
    }) | borderRounded
      | bgcolor(th.bg.has_color ? Color::RGB(th.bg.r, th.bg.g, th.bg.b)
                                 : Color::RGB(0, 0, 0))
      | color(Color::RGB(th.accent.r, th.accent.g, th.accent.b));

    return vbox({filler(), hbox({filler(), box})});
}

/* ── Song detail popup (key d) ───────────────────────── */
Element render_song_detail(const AppState &s) {
    if (!s.song_detail_open) return text("");
    auto &th = ThemeManager::instance().current();
    Elements body;
    for (const auto &l : s.song_detail_lines) {
        if (!l.empty() && l[0] == '\t')
            body.push_back(text(l.substr(1)) | color(Color::RGB(255,255,255)));
        else
            body.push_back(text(l));
    }
    auto box = vbox({
        text(" \u6B4C\u66F2\u8BE6\u60C5 ") | bold,   /* 歌曲详情 */
        separator(),
        vbox(std::move(body)),
    }) | borderRounded
      | bgcolor(th.bg.has_color ? Color::RGB(th.bg.r, th.bg.g, th.bg.b)
                                 : Color::RGB(0, 0, 0))
      | color(Color::RGB(th.accent.r, th.accent.g, th.accent.b));
    return vbox({filler(), hbox({filler(), box})});
}
