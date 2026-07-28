#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/components/spinner.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include "core/spectrum.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <algorithm>
using namespace ftxui;

static Element render_lyrics(const Lyrics *ly, int play_time_ms, int col_w) {
    if (!ly || ly->count == 0)
        return text("  No lyrics") | dim | center;

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

    float kprog = 0.0f;
    if (base + 1 < ly->count) {
        int dt = ly->lines[base + 1].time_ms - ly->lines[base].time_ms;
        if (dt > 0)
            kprog = (float)(play_time_ms - ly->lines[base].time_ms) / (float)dt;
        if (kprog < 0.0f) kprog = 0.0f;
        if (kprog > 1.0f) kprog = 1.0f;
    }

    const int max_text = col_w - 1;
    if (max_text < 4) return text("") | size(HEIGHT, EQUAL, 20);

    auto centered = [](Element e) {
        return hbox({filler(), std::move(e), filler()});
    };

    Elements lines;
    for (int i = 0; i < ly->count; i++) {
        std::string raw = ly->lines[i].text ? ly->lines[i].text : "";
        if (raw.empty()) raw = " ";

        if (string_width(raw) > max_text) {
            std::string out;
            int w = 0;
            int limit = max_text - 3;
            if (limit < 1) limit = 1;
            for (const auto &g : Utf8ToGlyphs(raw)) {
                int gw = string_width(g);
                if (w + gw > limit) break;
                out += g;
                w += gw;
            }
            raw = out + "...";
        }

        if (i == base) {
            int text_w = string_width(raw);
            int bar_len = (int)(kprog * (float)text_w);
            if (bar_len < 0) bar_len = 0;
            if (bar_len > text_w) bar_len = text_w;
            std::string bar_str;
            for (int j = 0; j < bar_len; j++) bar_str += "\u2501";

            lines.push_back(
                centered(vbox({
                    text(raw) | bold,
                    theme_accent(text(bar_str)),
                })) | focus
            );
        } else {
            lines.push_back(centered(theme_fg(text(raw)) | dim));
        }
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

    if (s.cover.pixels && s.cover.width > 0 && s.cover.height > 0)
        return render_cover(s.cover, cw) | center | flex;
    if (s.cover_loading)
        return render_spinner(s) | center | flex;
    return vbox({text("")}) | center | flex;
}

Element render_lyrics_only(const AppState &s) {
    int total = s.song_panel_width + 29;
    int cw = total / 2 - 1;
    if (cw < 12) cw = 12;
    if (cw > 60) cw = 60;
    int lw = total - cw - 1;
    if (lw < 20) lw = 20;
    return render_lyrics(s.lyrics, s.current_time_ms, lw) |
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

/* ── Spectrum bar (2 rows, 16-level bars) ──────────── */
Element render_spectrum_bar(const AppState &s) {
    /* UTF-8 bytes for each level: 0=space, 1-8=▁▂▃▄▅▆▇█ */
    static const char LEVEL_BYTES[9][4] = {
        {0x20, 0, 0, 0},
        {'\xe2','\x96','\x81', 0},
        {'\xe2','\x96','\x82', 0},
        {'\xe2','\x96','\x83', 0},
        {'\xe2','\x96','\x84', 0},
        {'\xe2','\x96','\x85', 0},
        {'\xe2','\x96','\x86', 0},
        {'\xe2','\x96','\x87', 0},
        {'\xe2','\x96','\x88', 0},
    };

    /* Full terminal width for this panel view */
    int total_w = s.song_panel_width + 29;
    if (total_w < 8) total_w = 8;
    if (total_w > SPECTRUM_BANDS) total_w = SPECTRUM_BANDS;

    /* Show real bands: one column per band, no stretching,
       at most as many as the terminal width allows */
    int bands = SPECTRUM_BANDS;
    if (bands > total_w) bands = total_w;

    /* ── Envelope follower + emphasis ────────────────── */
    /* Replaces the peak-normalization approach with a simple
       envelope that rises instantly and falls quickly.
       Pre-emphasis boosts high frequencies so they're visible
       despite music's natural HF roll-off. */
    static float s_env[SPECTRUM_BANDS] = {0};
    const float ENV_RELEASE = 0.92f;  /* ~150ms to fall 50% at 60fps */
    const float GAIN = 3.5f;
    const float FLOOR = 0.05f;

    std::string row1, row2;
    for (int i = 0; i < bands; i++) {
        float v = s.spectrum[i];
        if (v < 0.0f) v = 0.0f;

        /* Pre-emphasis: 0.8 + 12*(i/128)^2
           Band 0: 0.8x  Band 64: 3.8x  Band 127: 12.8x */
        float emphasis = 0.8f + 12.0f * (float)i * (float)i
                          / ((float)SPECTRUM_BANDS * (float)SPECTRUM_BANDS);
        v *= emphasis;

        /* Envelope follower: instant attack, fast release */
        if (v > s_env[i]) {
            s_env[i] = v;       /* instant rise */
        } else {
            s_env[i] *= ENV_RELEASE;  /* fast fall */
            if (s_env[i] < 0.001f) s_env[i] = 0.0f;
        }
        v = s_env[i];

        /* Gain + floor → visible bar */
        v = v * GAIN + FLOOR;
        if (v > 1.0f) v = 1.0f;

        /* Map to 2-row bar (16 levels) */
        int total = (int)(v * 16.0f + 0.5f);
        if (total > 16) total = 16;
        int bot = (total > 8) ? 8 : total;
        int top = (total > 8) ? (total - 8) : 0;
        row2.append(LEVEL_BYTES[bot], bot == 0 ? 1 : 3);
        row1.append(LEVEL_BYTES[top], top == 0 ? 1 : 3);
    }

    /* Build bar: always exactly 2 rows high, centered */
    Element bars;
    bool dimmed = (s.playback_state != PlaybackState::Playing);
    if (dimmed) {
        bars = vbox({
            text(row1) | dim,
            text(row2) | dim,
        });
    } else {
        bars = vbox({
            theme_spectrum(text(row1)),
            theme_spectrum(text(row2)),
        });
    }

    /* Center in available space */
    return hbox({
        filler(),
        bars,
        filler(),
    }) | size(HEIGHT, EQUAL, 2);
}
