#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <cstdio>
using namespace ftxui;

/* ── Render lyrics (right column) ──────────────────── */
static Element render_lyrics(const Lyrics *ly, int current_line) {
    if (!ly || ly->count == 0) {
        return text("  No lyrics") | dim | center;
    }

    Elements items;
    for (int i = 0; i < ly->count; i++) {
        std::string line_text = ly->lines[i].text ? ly->lines[i].text : "";

        if (i == current_line) {
            /* highlight current line */
            items.push_back(theme_accent(text("  " + line_text) | bold));
        } else if (i == current_line + 1 || i == current_line - 1) {
            /* adjacent lines slightly dim */
            items.push_back(theme_fg(text("  " + line_text)));
        } else {
            items.push_back(theme_fg(text("  " + line_text)) | dim);
        }
    }

    /* centering logic: try to put current line in the visual center */
    int pad_before = 0;
    /* Reserve enough space so current line is roughly centered */
    const int vis_lines = 20;  /* approximately half the terminal */
    if (current_line >= 0) {
        pad_before = vis_lines / 2 - current_line;
        if (pad_before < 0) pad_before = 0;
    }

    Elements padded;
    for (int i = 0; i < pad_before; i++)
        padded.push_back(text(""));
    for (auto &item : items)
        padded.push_back(std::move(item));
    padded.push_back(filler());

    return vbox(std::move(padded)) | frame | vscroll_indicator | flex;
}

/* ── Album art placeholder (left column) ──────────── */
static Element render_cover_placeholder() {
    Elements lines;
    lines.push_back(text(""));
    lines.push_back(text("  ") | bold);
    lines.push_back(text("  [ Cover ]") | dim | center);
    lines.push_back(text("  ") | bold);
    lines.push_back(text(""));

    /* Simple decorative box */
    auto box = vbox(std::move(lines));
    /* Surround with a border-like empty space */
    return box | center | border | flex;
}

/* ══════════════════════════════════════════════════
   Main render
   ══════════════════════════════════════════════════ */
Element render_lyric_panel(const AppState &s) {
    int current_line = -1;
    if (s.lyrics && s.lyrics->count > 0) {
        current_line = lyric_find_line(s.lyrics, s.current_time_sec * 1000);
    }

    auto left_col = render_cover_placeholder();
    auto right_col = render_lyrics(s.lyrics, current_line);

    return theme_bg(
        hbox({
            left_col | border,
            separator(),
            right_col | border,
        })
    );
}
