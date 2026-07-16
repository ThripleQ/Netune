#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
#include <vector>
using namespace ftxui;

/* ── Render lyrics ────────────────────────────────── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms) {
    if (!ly || ly->count == 0)
        return text("  No lyrics") | dim | center | flex;

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

    /* Window around current line */
    const int before = 6;
    const int after  = 12;
    int start = base - before;
    if (start < 0) start = 0;
    int end = base + after;
    if (end > ly->count) end = ly->count;

    Elements items;
    for (int i = start; i < end; i++) {
        std::string raw = ly->lines[i].text ? ly->lines[i].text : "";
        if (i == base) {
            items.push_back(theme_accent(text("  " + raw) | bold));
        } else if (i == base + 1 || i == base - 1) {
            items.push_back(theme_fg(text("  " + raw)));
        } else {
            items.push_back(theme_fg(text("  " + raw)) | dim);
        }
    }

    return vbox(std::move(items));
}

/* ── Cover placeholder ────────────────────────────── */
static Element render_cover() {
    return vbox({
        text("") | bold,
        text("  [ Cover ]") | dim | center,
        text("") | bold,
    }) | center | flex;
}

Element render_lyric_panel(const AppState &s) {
    int ms = s.current_time_sec * 1000;
    return theme_bg(hbox({
        render_cover(),
        render_lyrics(s.lyrics, ms),
    }));
}
