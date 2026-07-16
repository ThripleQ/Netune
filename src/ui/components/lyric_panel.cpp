#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
using namespace ftxui;

/* ── Current line: text + thin line progress ───── */
static Element current_line(const std::string &txt, float progress) {
    int w = 20;  /* fixed dot count for smooth animation */
    int filled = (int)(progress * (float)w);
    if (filled < 0) filled = 0;
    if (filled > w) filled = w;

    std::string bar;
    for (int i = 0; i < filled; i++)  bar += "\u2501";  /* ━ */

    return vbox({
        theme_accent(text("  " + txt) | bold),
        theme_accent(text("  " + bar)),
    });
}

/* ── Render lyrics ────────────────────────────────── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms, int max_width) {
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

    /* Truncate long lines to fit panel (rough estimate: available width - margins) */
    auto trunc = [](const std::string &s, int max_w) -> std::string {
        int w = string_width(s);
        if (w <= max_w) return s;
        /* find truncation point */
        int col = 0;
        size_t cut = 0;
        for (size_t i = 0; i < s.size() && col + 1 < max_w - 1; ) {
            unsigned char c = (unsigned char)s[i];
            int len = 1;
            int cw = 1;
            if ((c & 0xF0) == 0xF0) { len = 4; cw = 2; }
            else if ((c & 0xE0) == 0xE0) { len = 3; cw = 2; }
            else if ((c & 0xC0) == 0xC0) { len = 2; cw = 2; }
            col += cw;
            cut = i + len;
            i += len;
        }
        return s.substr(0, cut) + "\u2026";
    };

    if (max_width < 20) max_width = 20;

    for (int i = start; i < end; i++) {
        std::string raw = ly->lines[i].text ? ly->lines[i].text : "";
        std::string display = trunc(raw, lyrics_w);
        if (i == base) {
            items.push_back(current_line(display, kprog));
        } else if (i == base + 1 || i == base - 1) {
            items.push_back(theme_fg(text("  " + display)));
        } else {
            items.push_back(theme_fg(text("  " + display)) | dim);
        }
    }

    items.push_back(filler());
    return vbox(std::move(items));
}

/* ── Cover: ▄ half-block rendering, dynamic resize ─ */
static Element render_cover(const CoverData &cd, int panel_width_chars) {
    if (!cd.pixels || cd.width <= 0 || cd.height <= 0 || panel_width_chars < 4) {
        return vbox({
            text("") | bold,
            text("  [ Cover ]") | dim | center,
            text("") | bold,
        }) | center | flex;
    }

    /* Target: fit panel_width_chars columns, keep aspect ratio */
    int dw = panel_width_chars;
    if (dw > cd.width) dw = cd.width;
    int dh = cd.height * dw / cd.width;
    if (dh % 2) dh++;  /* even rows for ▄ pairs */

    /* NN-scale + render in one pass */
    int src_w = cd.width, src_h = cd.height;
    Elements rows;
    for (int y = 0; y < dh; y += 2) {
        Elements cells;
        for (int x = 0; x < dw; x++) {
            int sx = x * src_w / dw;
            int sy0 = (y    ) * src_h / dh;
            int sy1 = (y + 1) * src_h / dh;
            if (sy1 >= src_h) sy1 = sy0;
            int top = (sy0 * src_w + sx) * 3;
            int bot = (sy1 * src_w + sx) * 3;
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

Element render_lyric_panel(const AppState &s) {
    int ms = s.current_time_ms;
    /* Cover panel width: roughly half the screen minus lyrics side */
    int cover_w = s.song_panel_width > 20 ? (s.song_panel_width - 5) / 2 : 15;
    if (cover_w < 10) cover_w = 10;
    if (cover_w > 35) cover_w = 35;
    /* Lyrics panel gets the rest, minus margins */
    int lyrics_w = s.song_panel_width - cover_w - 4;
    if (lyrics_w < 20) lyrics_w = 20;
    if (lyrics_w > 80) lyrics_w = 80;
    return theme_bg(hbox({
        render_cover(s.cover, cover_w) | size(WIDTH, EQUAL, cover_w),
        render_lyrics(s.lyrics, ms, lyrics_w) | flex,
    }));
}
