#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
using namespace ftxui;

/* ── Render lyrics: 20 rows fixed, text only no wrap ─────────────── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms, int col_w) {
    if (!ly || ly->count == 0)
        return text("  No lyrics") | dim | center | flex;

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;
    int total = ly->count;

    /* Progress % for bar on current line */
    float kprog = 0.0f;
    if (base + 1 < total) {
        int dt = ly->lines[base + 1].time_ms - ly->lines[base].time_ms;
        if (dt > 0)
            kprog = (float)(play_time_ms - ly->lines[base].time_ms) / (float)dt;
        if (kprog < 0.0f) kprog = 0.0f;
        if (kprog > 1.0f) kprog = 1.0f;
    }

    /* Window: 20 rows. 4 empty rows above current, rest lyrics. */
    const int W = 20;
    const int above = 4;
    const int lyric_rows = W - above;  /* 16 lyrics visible */

    /* Which lyrics to show? */
    int start = base - above;
    if (start < 0) start = 0;
    int end = start + W;
    if (end > total) { end = total; start = end - W; if (start < 0) start = 0; }

    /* Lead empty lines: push current line to position `above` from top */
    int lead = base - start;
    if (lead > above) lead = above;
    if (lead < 0) lead = 0;

    Elements items;
    for (int i = 0; i < lead; i++)
        items.push_back(text(""));

    for (int i = start; i < end; i++) {
        std::string raw = ly->lines[i].text ? ly->lines[i].text : "";
        if (i == base) {
            /* Current line: text + ━━ progress appended after text */
            int avail = col_w - 4 - (int)raw.size();
            if (avail < 1) avail = 1;
            int filled = (int)(kprog * (float)avail);
            if (filled < 0) filled = 0;
            if (filled > avail) filled = avail;
            std::string bar; for (int j = 0; j < filled; j++) bar += "\u2501";
            items.push_back(theme_accent(text(std::string("  ") + raw + bar) | bold));
        } else if (i == base + 1 || i == base - 1) {
            items.push_back(theme_fg(text("  " + raw)));
        } else {
            items.push_back(theme_fg(text("  " + raw)) | dim);
        }
    }

    /* Trail: fill to exactly W rows */
    while ((int)items.size() < W)
        items.push_back(text(""));

    return vbox(std::move(items));
}

/* ── Cover: ▄ half-block ──────────────────────────────────────────── */
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
            int sy0 = (y    ) * sh / dh;
            int sy1 = (y + 1) * sh / dh;
            if (sy1 >= sh) sy1 = sy0;
            int top3 = (sy0 * sw + sx) * 3;
            int bot3 = (sy1 * sw + sx) * 3;
            cells.push_back(bgcolor(
                Color::RGB(cd.pixels[top3], cd.pixels[top3+1], cd.pixels[top3+2]),
                color(Color::RGB(cd.pixels[bot3], cd.pixels[bot3+1], cd.pixels[bot3+2]),
                    text("\u2580"))
            ));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(rows)) | center | flex;
}

/* ── Exported ─────────────────────────────────────────────────────── */
Element render_cover_only(const AppState &s) {
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
    int lyrics_w = total - cover_w - 2;
    if (lyrics_w < 20) lyrics_w = 20;
    /* Fixed 21 rows (1 empty + 20 lyrics), centered in column */
    return vbox({text(""), render_lyrics(s.lyrics, ms, lyrics_w)}) | center;
}

Element render_lyric_panel(const AppState &s) {
    int total = s.song_panel_width + 29;
    int cover_w = total / 2 - 1;
    if (cover_w < 12) cover_w = 12;
    if (cover_w > 60) cover_w = 60;
    int lyrics_w = total - cover_w - 2;
    if (lyrics_w < 20) lyrics_w = 20;
    return theme_bg(hbox(Elements{
        render_cover_only(s) | size(WIDTH, EQUAL, cover_w),
        render_lyrics_only(s) | size(WIDTH, EQUAL, lyrics_w),
    }));
}
