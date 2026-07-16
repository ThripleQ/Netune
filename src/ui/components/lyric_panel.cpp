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

/* ── Cover: ▄ half-block pixel rendering ───────── */
static Element render_cover(const CoverData &cd) {
    if (!cd.pixels || cd.width <= 0 || cd.height <= 0) {
        return vbox({
            text("") | bold,
            text("  [ Cover ]") | dim | center,
            text("") | bold,
        }) | center | flex;
    }

    /* Each ▄ renders 2 pixels: top=bg, bottom=fg */
    int hh = cd.height / 2;  /* half the rows (2 pixels per row) */
    Elements rows;
    for (int y = 0; y < hh; y++) {
        Elements cells;
        for (int x = 0; x < cd.width; x++) {
            int top = ((y * 2    ) * cd.width + x) * 3;
            int bot = ((y * 2 + 1) * cd.width + x) * 3;
            if (bot >= cd.width * cd.height * 3) bot = top;
            Color top_c = Color::RGB(cd.pixels[top], cd.pixels[top+1], cd.pixels[top+2]);
            Color bot_c = Color::RGB(cd.pixels[bot], cd.pixels[bot+1], cd.pixels[bot+2]);
            cells.push_back(
                bgcolor(top_c, color(bot_c, text("\u2580")))
            );
        }
        rows.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(rows)) | center | flex;
}

Element render_lyric_panel(const AppState &s) {
    int ms = s.current_time_ms;
    return theme_bg(hbox({
        render_cover(s.cover),
        render_lyrics(s.lyrics, ms),
    }));
}
