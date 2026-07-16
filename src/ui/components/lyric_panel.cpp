#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
using namespace ftxui;

/*
 * Simplest possible lyrics panel: 20 fixed rows.
 * Lines 0-3 are empty (padding), line 4 is the current lyric position.
 * The current line is highlighted. No progress bar, no wrap, no centering tricks.
 * The lyrics window slides through the song as base moves.
 */

static Element render_lyrics(const Lyrics *ly, int play_time_ms, int col_w) {
    if (!ly || ly->count == 0)
        return text("");

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

    const int ROWS = 20;
    const int CUR_POS = 4;
    std::string blank((size_t)col_w, ' ');  /* clear previous text */

    Elements items;
    for (int i = 0; i < ROWS; i++) {
        int ni = i - CUR_POS + base;
        if (ni < 0 || ni >= ly->count) {
            items.push_back(text(blank));
            continue;
        }
        std::string raw = ly->lines[ni].text ? ly->lines[ni].text : "";
        if (raw.empty()) {
            items.push_back(text(blank));
        } else if (ni == base) {
            items.push_back(theme_accent(text("  " + raw) | bold));
        } else if (ni == base + 1 || ni == base - 1) {
            items.push_back(theme_fg(text("  " + raw)));
        } else {
            items.push_back(theme_fg(text("  " + raw)) | dim);
        }
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
