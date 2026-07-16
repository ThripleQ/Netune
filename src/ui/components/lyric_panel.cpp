#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
using namespace ftxui;

/* ── Current line: text + thin line progress ───── */
static Element current_line(const std::string &txt, float progress) {
    int w = string_width(txt);
    if (w > 50) w = 50;
    int filled = (int)(progress * (float)w);
    if (filled < 0) filled = 0;
    if (filled > w) filled = w;

    std::string bar;
    for (int i = 0; i < filled; i++)  bar += "\u2504";  /* ┄ */

    return vbox({
        theme_accent(text("  " + txt) | bold),
        theme_accent(text("  " + bar)),
    });
}

/* ── Render lyrics ────────────────────────────────── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms) {
    if (!ly || ly->count == 0)
        return text("  No lyrics") | dim | center | flex;

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

    /* Progress within the current line */
    float kprog = 0.0f;
    if (base + 1 < ly->count) {
        int t0 = ly->lines[base].time_ms;
        int t1 = ly->lines[base + 1].time_ms;
        int dt = t1 - t0;
        if (dt > 0)
            kprog = (float)(play_time_ms - t0) / (float)dt;
        if (kprog < 0.0f) kprog = 0.0f;
        if (kprog > 1.0f) kprog = 1.0f;
    }

    /* Window around current line */
    const int above = 4;
    const int below = 20;
    int start = base - above;
    if (start < 0) start = 0;
    int end = base + below;
    if (end > ly->count) end = ly->count;

    Elements items;
    int top_pad = (start > 0 && base - start < above) ? (above - (base - start)) : 0;
    for (int i = 0; i < top_pad; i++)
        items.push_back(text(""));

    for (int i = start; i < end; i++) {
        std::string raw = ly->lines[i].text ? ly->lines[i].text : "";
        if (i == base) {
            items.push_back(current_line(raw, kprog));
        } else if (i == base + 1 || i == base - 1) {
            items.push_back(theme_fg(text("  " + raw)));
        } else {
            items.push_back(theme_fg(text("  " + raw)) | dim);
        }
    }

    items.push_back(filler());
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
