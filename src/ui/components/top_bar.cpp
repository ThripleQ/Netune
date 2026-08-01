#include "ui/components/top_bar.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "compat/wcwidth_compat.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <cwchar>
using namespace ftxui;

/* ── Top search row (netease mode) ────────────────────
   A single line in the top_bar slot: two boxes delineated
   by │ side bars, separated by ││ at the panel boundary.
   No top/bottom borders — the panels below keep their own. */

/* truncate text to fit `width` display columns (UTF-8 safe) */
static std::string truncate_to(const std::string &text, int width) {
    int w = string_width(text);
    if (w <= width) return text;
    std::string out;
    int col = 0;
    std::mbstate_t st = {};
    for (size_t i = 0; i < text.size(); ) {
        wchar_t wc = 0;
        size_t rc = mbrtowc(&wc, text.data() + i, text.size() - i, &st);
        if (rc == 0 || rc == (size_t)-1 || rc == (size_t)-2) break;
        int cw = compat_wcwidth(wc); if (cw < 0) cw = 1;
        if (col + cw > width - 1) break;
        out.append(text, i, rc); col += cw; i += rc;
    }
    out += "\u2026";  /* … */
    return out;
}

/* One search box: a single "│…│" line. */
static Element search_box(const AppState &s, int side,
                          const std::string &query, const char *hint,
                          int width) {
    bool active = s.top_search_active && s.top_search_side == side;
    if (width < 4) width = 4;

    std::string content;
    if (query.empty())
        content = std::string(" ") + hint;
    else
        content = query;
    if (active) content += "\u258C";  /* cursor block */

    int inner = width - 2;
    int cw = string_width(content);
    if (cw > inner) {
        content = truncate_to(content, inner);
        cw = string_width(content);
    }
    if (cw < inner) content += std::string((size_t)(inner - cw), ' ');

    std::string row = "\u2502" + content + "\u2502";  /* │…│ */
    if (active)
        return theme_selection(text(row));
    return theme_bg(theme_fg(text(row)) | dim);
}

Element render_top_bar(const AppState &s) {
    int total = s.top_row_width > 0 ? s.top_row_width : 80;

    /* search row in both modes; lyric mode disables it */
    bool searchable = !s.lyric_mode;
    if (!searchable)
        return theme_bg(theme_fg(text(std::string((size_t)total, ' '))));

    const char *right_hint = (s.music_mode == MusicMode::Netease)
                                 ? "[/] 搜索歌曲 / 网易云"
                                 : "[/] 搜索歌曲";
    int right_w = total - 20;  /* left box is 20 cols, same as left panel */
    return hbox({
        search_box(s, 0, s.top_left_query, "[/] 搜索菜单", 20),
        search_box(s, 1, s.top_right_query, right_hint, right_w),
    });
}
