#include "ui/components/action_sheet.h"
#include "ui/components/theme_util.h"
#include "ui/theme.h"
#include <string>
using namespace ftxui;

Element render_action_sheet(const AppState &s) {
    if (!s.action_sheet_open) return text("");

    const auto &item = s.playlist.empty() ? SongInfo{} :
                       s.playlist[s.selected_index];
    bool is_playlist = item.is_playlist;
    std::string title = item.title ? item.title : "(nothing selected)";

    /* single toggling button: shows the current state and flips on Enter */
    std::string btn;
    if (s.action_sheet_active < 0) {
        btn = " \u2026 \u67E5\u8BE2\u72B6\u6001... ";   /* … 查询状态... */
    } else if (is_playlist) {
        btn = s.action_sheet_active == 1
            ? " \u2606 \u53D6\u6D88\u6536\u85CF\u6B4C\u5355 "   /* ☆ 取消收藏歌单 */
            : " \u2606 \u6536\u85CF\u6B4C\u5355 ";              /* ☆ 收藏歌单 */
    } else {
        btn = s.action_sheet_active == 1
            ? " \u2665 \u53D6\u6D88\u559C\u6B22 "   /* ♥ 取消喜欢 */
            : " \u2665 \u559C\u6B22\u6B64\u6B4C\u66F2 ";  /* ♥ 喜欢此歌曲 */
    }

    auto &th = ThemeManager::instance().current();
    auto box = vbox({
        text(" " + title + " ") | bold,
        separator(),
        hbox({
            text(" \u203A "),   /* ▸ */
            text(btn),
        }) | bold
          | bgcolor(Color::RGB(th.accent.r, th.accent.g, th.accent.b))
          | color(Color::RGB(0, 0, 0)),
        separator(),
        text(" Enter \u6267\u884C  Esc \u5173\u95ED ") | dim,
    }) | borderRounded
      | bgcolor(Color::RGB(0, 0, 0))            /* opaque backdrop */
      | color(Color::RGB(th.accent.r, th.accent.g, th.accent.b));

    /* bottom-right corner placement */
    return vbox({filler(), hbox({filler(), box})});
}
