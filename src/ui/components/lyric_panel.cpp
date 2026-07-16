#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
using namespace ftxui;

/* ── Current line: text + ━━ on same line ─────────── */
static Element current_line(const std::string &txt, float progress, int max_w) {
    int bar_cnt = (int)(progress * (float)(max_w - 4));
    if (bar_cnt < 0) bar_cnt = 0;
    std::string bar;
    for (int i = 0; i < bar_cnt; i++) bar += "\u2501";
    return theme_accent(hbox({
        text("  " + txt),
        text(bar) | flex,
    }) | bold);
}

/* ── Render lyrics: 20 lines, fixed, vertically centered ──── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms, int col_w) {
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

    /* 20-row window: lead + window_items = 20 always */
    const int window = 20;
    const int above = 4;
    int lead = (base < above) ? base : above;
    int start = base - lead;
    int end = start + (window - lead);
    if (end > ly->count) { end = ly->count; start = end - (window - lead); if (start < 0) start = 0; lead = base - start; if (lead < 0) lead = 0; }

    Elements items;
    for (int i = 0; i < lead; i++)
        items.push_back(text(""));

    for (int i = start; i < end; i++) {
        std::string raw = ly->lines[i].text ? ly->lines[i].text : "";
        if (i == base)
            items.push_back(current_line(raw, kprog, col_w));
        else if (i == base + 1 || i == base - 1)
            items.push_back(theme_fg(text("  " + raw)));
        else
            items.push_back(theme_fg(text("  " + raw)) | dim);
    }
    for (int i = (int)items.size(); i < window; i++)
        items.push_back(text(""));

    return vbox(std::move(items));
}

/* ── Cover: ▄ half-block ──────────────────────────── */
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

/* ── Exported ─────────────────────────────────────── */
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
