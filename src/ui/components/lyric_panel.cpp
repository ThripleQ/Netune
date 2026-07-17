#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
using namespace ftxui;

static Element render_lyrics(const Lyrics *ly, int play_time_ms) {
    if (!ly || ly->count == 0)
        return text("  No lyrics") | dim | center;

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

    /* Progress % for current line */
    float kprog = 0.0f;
    if (base + 1 < ly->count) {
        int dt = ly->lines[base + 1].time_ms - ly->lines[base].time_ms;
        if (dt > 0)
            kprog = (float)(play_time_ms - ly->lines[base].time_ms) / (float)dt;
        if (kprog < 0.0f) kprog = 0.0f;
        if (kprog > 1.0f) kprog = 1.0f;
    }

    Elements lines;
    for (int i = 0; i < ly->count; i++) {
        std::string raw = ly->lines[i].text ? ly->lines[i].text : "";
        if (raw.empty()) raw = " ";

        Element el = text("  " + raw);

        if (i == base) {
            /* Current line: text + gauge on same row */
            el = theme_accent(hbox({
                text("  " + raw),
                gauge(kprog) | flex,
            }) | bold) | focus;
        } else if (i == base + 1 || i == base - 1) {
            el = theme_fg(el);
        } else {
            el = theme_fg(el) | dim;
        }

        lines.push_back(el);
    }

    return vbox(std::move(lines)) | yframe;
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
    return render_lyrics(s.lyrics, s.current_time_ms) |
           size(WIDTH, EQUAL, lw) | size(HEIGHT, EQUAL, 20);
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
