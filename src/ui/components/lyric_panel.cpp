#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <ftxui/dom/canvas.hpp>
#include <string>
using namespace ftxui;

/* One canvas row: fixed col_w, clear every cell, draw text at indent 2 */
static Element canvas_row(int w, const std::string &text) {
    return canvas(w, 1, [w, text](Canvas &c) {
        c.DrawText(0, 0, std::string((size_t)w, ' '));
        if (!text.empty())
            c.DrawText(2, 0, text);
    });
}

/* Blank row: canvas same size, no text */
static Element blank_row(int w) {
    return canvas(w, 1, [w](Canvas &c) {
        c.DrawText(0, 0, std::string((size_t)w, ' '));
    });
}

static Element render_lyrics(const Lyrics *ly, int play_time_ms, int col_w) {
    if (!ly || ly->count == 0) {
        Elements rows(20);
        for (int i = 0; i < 20; i++)
            rows[i] = blank_row(col_w);
        return vbox(std::move(rows));
    }

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

    const int ROWS = 20;
    const int CUR = 4;

    Elements items;
    for (int i = 0; i < ROWS; i++) {
        int ni = i - CUR + base;

        if (ni < 0 || ni >= ly->count) {
            /* Canvas blank — same element type as every other row */
            items.push_back(blank_row(col_w));
            continue;
        }

        std::string raw = ly->lines[ni].text ? ly->lines[ni].text : "";
        Element el = canvas_row(col_w, raw);

        if (ni == base)
            el = theme_accent(el | bold);
        else if (ni == base + 1 || ni == base - 1)
            el = theme_fg(el);
        else
            el = theme_fg(el) | dim;

        items.push_back(el);
    }
    return vbox(std::move(items));
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
        render_lyrics_only(s) | size(WIDTH, EQUAL, lw),
    }));
}
