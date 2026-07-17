#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/theme.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <ftxui/dom/canvas.hpp>
#include <string>
using namespace ftxui;

/* ── Theme color helpers for Canvas ─────────────────────── */
static Color accent_col() {
    auto &t = ThemeManager::instance().current();
    return Color::RGB(t.accent.r, t.accent.g, t.accent.b);
}
static Color fg_col() {
    auto &t = ThemeManager::instance().current();
    return Color::RGB(t.fg.r, t.fg.g, t.fg.b);
}

/* ── Lyrics: one Canvas, pixel-level smooth scrolling ──── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms, int col_w) {
    const int ROWS = 20;
    const int CUR = 4;  /* current line visual row index */

    if (!ly || ly->count == 0) {
        /* blank canvas — background color from theming fills it */
        return canvas(col_w, ROWS, [](Canvas &) {});
    }

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

    /* Smooth scroll: 0..4 y-units */
    float kprog = 0.0f;
    if (base + 1 < ly->count) {
        int dt = ly->lines[base + 1].time_ms - ly->lines[base].time_ms;
        if (dt > 0)
            kprog = (float)(play_time_ms - ly->lines[base].time_ms) / (float)dt;
        if (kprog < 0.0f) kprog = 0.0f;
        if (kprog > 1.0f) kprog = 1.0f;
    }
    int scroll_off = (int)(kprog * 4.0f);  /* 0..4 */

    return canvas(col_w, ROWS, [=](Canvas &c) {
        auto accent = accent_col();
        auto fg = fg_col();

        /* fill entire canvas with spaces to erase old content */
        std::string fill((size_t)col_w, ' ');
        for (int r = 0; r < ROWS; r++)
            c.DrawText(0, r * 4, fill);

        for (int i = 0; i < ROWS; i++) {
            int ni = i - CUR + base;
            int y = i * 4 - scroll_off;

            std::string raw;
            if (ni >= 0 && ni < ly->count && ly->lines[ni].text)
                raw = ly->lines[ni].text;
            if (raw.empty()) continue;

            if (ni == base)
                c.DrawText(4, y, raw, [&accent](Pixel &p) {
                    p.foreground_color = accent;
                    p.bold = true;
                });
            else if (ni == base + 1 || ni == base - 1)
                c.DrawText(4, y, raw, fg);
            else
                c.DrawText(4, y, raw, [&fg](Pixel &p) {
                    p.foreground_color = fg;
                    p.dim = true;
                });
        }
    });
}

/* ── Cover ───────────────────────────────────────────────── */
static Element render_cover(const CoverData &cd, int panel_w) {
    if (!cd.pixels || cd.width <= 0 || cd.height <= 0 || panel_w < 4)
        return vbox({text("")}) | center | flex;

    int dw = panel_w;
    if (dw > cd.width) dw = cd.width;
    int dh = cd.height * dw / cd.width;
    if (dh % 2) dh++;
    int sw = cd.width, sh = cd.height;
    Elements rows;
    for (int y = 0; y < dh; y += 2) {
        Elements cells;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            int sy0 = (y    ) * sh / dh, sy1 = (y + 1) * sh / dh;
            if (sy1 >= sh) sy1 = sy0;
            int t = (sy0 * sw + sx) * 3, b = (sy1 * sw + sx) * 3;
            cells.push_back(bgcolor(
                Color::RGB(cd.pixels[t], cd.pixels[t+1], cd.pixels[t+2]),
                color(Color::RGB(cd.pixels[b], cd.pixels[b+1], cd.pixels[b+2]),
                    text("\u2580"))));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(rows)) | center | flex;
}

/* ── Exported ─────────────────────────────────────────────── */
Element render_cover_only(const AppState &s) {
    int total = s.song_panel_width + 29;
    int cw = total / 2 - 1;
    if (cw < 12) cw = 12;
    if (cw > 60) cw = 60;
    return render_cover(s.cover, cw) | center | flex;
}

Element render_lyrics_only(const AppState &s) {
    int total = s.song_panel_width + 29;
    int cw = total / 2 - 1;
    if (cw < 12) cw = 12;
    if (cw > 60) cw = 60;
    int lw = total - cw - 1;
    if (lw < 20) lw = 20;
    return render_lyrics(s.lyrics, s.current_time_ms, lw);
}

Element render_lyric_panel(const AppState &s) {
    int total = s.song_panel_width + 29;
    int cw = total / 2 - 1;
    if (cw < 12) cw = 12;
    if (cw > 60) cw = 60;
    int lw = total - cw - 1;
    if (lw < 20) lw = 20;
    return theme_bg(hbox(Elements{
        render_cover_only(s) | size(WIDTH, EQUAL, cw),
        render_lyrics_only(s) | size(WIDTH, EQUAL, lw) | center,
    }));
}
