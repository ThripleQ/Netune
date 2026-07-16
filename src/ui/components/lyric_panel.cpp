#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
using namespace ftxui;

/* ── Render lyrics column ─────────────────────────── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms) {
    if (!ly || ly->count == 0) {
        return text("  No lyrics") | dim | center | flex;
    }

    int base_line = lyric_find_line(ly, play_time_ms);
    if (base_line < 0) base_line = 0;

    /* Build lines — focus on current so FTXUI auto-scrolls upward */
    Elements items;
    for (int i = 0; i < ly->count; i++) {
        std::string line_text = ly->lines[i].text ? ly->lines[i].text : "";
        auto el = text("  " + line_text);
        if (i == base_line) {
            items.push_back(theme_accent(el | bold) | focus);
        } else if (i == base_line + 1 || i == base_line - 1) {
            items.push_back(theme_fg(el));
        } else {
            items.push_back(theme_fg(el) | dim);
        }
    }

    return vbox(std::move(items)) | frame | vscroll_indicator | flex;
}

/* ── Cover placeholder (left, quiet) ──────────────── */
static Element render_cover() {
    return vbox({
        text("") | bold,
        text("  [ Cover ]") | dim | center,
        text("") | bold,
    }) | center | flex;
}

/* ══════════════════════════════════════════════════
   Full lyrics view
   ══════════════════════════════════════════════════ */
Element render_lyric_panel(const AppState &s) {
    int play_ms = s.current_time_sec * 1000;
    return theme_bg(hbox({
        render_cover(),
        render_lyrics(s.lyrics, play_ms),
    }));
}
