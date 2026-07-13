#include "ui/components/search_bar.h"
#include "ui/components/theme_util.h"
#include <cstdio>
using namespace ftxui;

Element render_search_bar(const AppState &s) {
    if (!s.search_active) return text("");

    /* ── Input line ─────────────────────────────────── */
    const char *mode_tag = (s.music_mode == MusicMode::Netease) ? "Netease" : "Local";
    std::string input_line = std::string(" [/] ") + mode_tag + " > " + s.search_query + "\u258C";

    Elements row;
    row.push_back(theme_accent(text(input_line)));

    /* ── Local: show results inline in the bar ──────── */
    if (s.music_mode != MusicMode::Netease && !s.search_results.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "  %d/%d",
                 (int)s.search_results.size(), s.search_total);
        row.push_back(text(buf) | dim);
    }

    if (s.music_mode == MusicMode::Netease && !s.search_query.empty())
        row.push_back(text("  [Enter] search") | dim);

    return hbox(std::move(row)) | bgcolor(Color::RGB(20, 20, 30)) | border | clear_under;
}
