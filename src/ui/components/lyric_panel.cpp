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

    /* Calculate float scroll position via interpolation */
    int base_line = lyric_find_line(ly, play_time_ms);
    float scroll_pos = (float)base_line;
    if (base_line >= 0 && base_line + 1 < ly->count) {
        int t0 = ly->lines[base_line].time_ms;
        int t1 = ly->lines[base_line + 1].time_ms;
        int dt = t1 - t0;
        if (dt > 0) {
            float t = (float)(play_time_ms - t0) / (float)dt;
            if (t > 0.0f && t < 1.0f)
                scroll_pos += t;
        }
    }
    if (scroll_pos < 0) scroll_pos = 0;

    /* Build lines */
    Elements items;
    for (int i = 0; i < ly->count; i++) {
        std::string line_text = ly->lines[i].text ? ly->lines[i].text : "";
        if (i == base_line) {
            items.push_back(theme_accent(text("  " + line_text) | bold));
        } else if (i == base_line + 1 || i == base_line - 1) {
            items.push_back(theme_fg(text("  " + line_text)));
        } else {
            items.push_back(theme_fg(text("  " + line_text)) | dim);
        }
    }

    /* Smooth scroll: show `scroll_pos` lines above the viewport */
    Elements padded;
    int pad = (int)scroll_pos;
    for (int i = 0; i < pad; i++)
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
   Full lyrics view
   ══════════════════════════════════════════════════ */
Element render_lyric_panel(const AppState &s) {
    int play_ms = s.current_time_sec * 1000;
    return theme_bg(hbox({
        render_cover(),
        render_lyrics(s.lyrics, play_ms),
    }));
}
