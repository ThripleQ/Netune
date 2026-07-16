#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
#include <vector>
using namespace ftxui;

/* ── CJK-aware wrap: each CJK char wraps, ASCII stays word-level ── */
static Element wrap(const std::string &s) {
    Elements cells;
    std::string word;
    auto flush = [&] { if (!word.empty()) { cells.push_back(text(word)); word.clear(); } };
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ') { flush(); cells.push_back(text(" ")); i++; continue; }
        if ((c & 0x80) == 0) { word += c; i++; continue; }
        int len = 1;
        if      ((c & 0xF0) == 0xF0) len = 4;
        else if ((c & 0xE0) == 0xE0) len = 3;
        else if ((c & 0xC0) == 0xC0) len = 2;
        flush();
        cells.push_back(text(s.substr(i, (size_t)len)));
        i += (size_t)len;
    }
    flush();
    return flexbox(std::move(cells), FlexboxConfig().SetGap(0, 0));
}

/* ── Current line: wrapped text + ━━ progress bar ─ */
static Element current_line(const std::string &txt, float progress, int panel_w) {
    /* Bar capped to panel width (minus indent). If text wraps, bar won't overflow */
    int bar_max = string_width(txt);
    int avail = panel_w - 2;
    if (bar_max > avail) bar_max = avail;
    if (bar_max < 1) bar_max = 1;

    int filled = (int)(progress * (float)bar_max);
    if (filled < 0) filled = 0;
    if (filled > bar_max) filled = bar_max;

    std::string bar;
    for (int i = 0; i < filled; i++) bar += "\u2501";

    return vbox({
        theme_accent(wrap("  " + txt) | bold),
        theme_accent(text("  " + bar)),
    });
}

/* ── Render lyrics ────────────────────────────────── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms, int panel_w) {
    if (!ly || ly->count == 0)
        return text("  No lyrics") | dim | center | flex;

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

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

    const int above = 4, below = 15;  /* total 20 rows = cover height */
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
            items.push_back(current_line(raw, kprog, panel_w));
        } else if (i == base + 1 || i == base - 1) {
            items.push_back(theme_fg(wrap("  " + raw)));
        } else {
            items.push_back(theme_fg(wrap("  " + raw)) | dim);
        }
    }

    return vbox(std::move(items));
}

/* ── Cover: ▄ half-block rendering ───────────────── */
static Element render_cover(const CoverData &cd, int panel_w) {
    if (!cd.pixels || cd.width <= 0 || cd.height <= 0 || panel_w < 4)
        return vbox({
            text("") | bold,
            text("  [ Cover ]") | dim | center,
            text("") | bold,
        }) | center | flex;

    int dw = panel_w;
    if (dw > cd.width) dw = cd.width;  /* respect stored res cap */
    int dh = cd.height * dw / cd.width;
    if (dh % 2) dh++;
    int sw = cd.width, sh = cd.height;

    Elements rows;
    for (int y = 0; y < dh; y += 2) {
        Elements cells;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            int sy0 = (y    ) * sh / dh;
            int sy1 = (y + 1) * sh / dh;
            if (sy1 >= sh) sy1 = sy0;
            int top = (sy0 * sw + sx) * 3;
            int bot = (sy1 * sw + sx) * 3;
            cells.push_back(bgcolor(
                Color::RGB(cd.pixels[top], cd.pixels[top+1], cd.pixels[top+2]),
                color(Color::RGB(cd.pixels[bot], cd.pixels[bot+1], cd.pixels[bot+2]),
                    text("\u2580"))
            ));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(rows)) | center | flex;
}

Element render_cover_only(const AppState &s) {
    /* Actual terminal width = song_panel_width + 29 (left column) */
    int total = s.song_panel_width + 29;
    int cover_w = total / 2 - 1;
    if (cover_w < 12) cover_w = 12;
    if (cover_w > 60) cover_w = 60;
    return render_cover(s.cover, cover_w) | center | flex;
}

Element render_lyrics_only(const AppState &s) {
    int ms = s.current_time_ms;
    int total = s.song_panel_width + 29;
    int cover_w = total / 2 - 1;
    if (cover_w < 12) cover_w = 12;
    if (cover_w > 60) cover_w = 60;
    int lyrics_w = total - cover_w - 3;
    if (lyrics_w < 20) lyrics_w = 20;
    return vbox({text(""), render_lyrics(s.lyrics, ms, lyrics_w)});  /* top pad */
}

Element render_lyric_panel(const AppState &s) {
    return theme_bg(hbox(Elements{
        render_cover_only(s) | flex,
        render_lyrics_only(s) | flex,
    }));
}
