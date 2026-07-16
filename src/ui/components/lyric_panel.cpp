#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
using namespace ftxui;

/* ── Render lyrics column ─────────────────────────── */
static Element render_lyrics(const Lyrics *ly, int current_line) {
    if (!ly || ly->count == 0) {
        return text("  No lyrics") | dim | center | flex;
    }

    Elements items;
    for (int i = 0; i < ly->count; i++) {
        std::string line_text = ly->lines[i].text ? ly->lines[i].text : "";
        if (i == current_line)
            items.push_back(theme_accent(text("  " + line_text) | bold));
        else if (i == current_line + 1 || i == current_line - 1)
            items.push_back(theme_fg(text("  " + line_text)));
        else
            items.push_back(theme_fg(text("  " + line_text)) | dim);
    }

    /* Pad so current line is roughly centered */
    Elements padded;
    int pad_top = 8 - current_line;
    for (int i = 0; i < pad_top && i < 8; i++)
        padded.push_back(text(""));
    for (auto &item : items)
        padded.push_back(std::move(item));
    padded.push_back(filler());

    return vbox(std::move(padded)) | frame | vscroll_indicator | flex;
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
   Full lyrics view — no borders, no separator
   ══════════════════════════════════════════════════ */
Element render_lyric_panel(const AppState &s) {
    int current_line = -1;
    if (s.lyrics && s.lyrics->count > 0)
        current_line = lyric_find_line(s.lyrics, s.current_time_sec * 1000);

    return theme_bg(hbox({
        render_cover(),
        render_lyrics(s.lyrics, current_line),
    }));
}
