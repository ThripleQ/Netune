#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/components/spinner.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include "core/spectrum.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
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

/* ── Pre-computed gradient LUT (indexed by band i) ── */
struct GradientRGB { uint8_t r, g, b; };
static GradientRGB s_bot_lut[SPECTRUM_BANDS];
static GradientRGB s_top_lut[SPECTRUM_BANDS];

static void init_gradient_lut(void) {
    const auto &theme = ThemeManager::instance().current();
    uint8_t base_r = theme.spectrum.has_color ? theme.spectrum.r : theme.accent.r;
    uint8_t base_g = theme.spectrum.has_color ? theme.spectrum.g : theme.accent.g;
    uint8_t base_b = theme.spectrum.has_color ? theme.spectrum.b : theme.accent.b;

    const float MAX_HBLEND = 0.45f;
    float mid = (float)(SPECTRUM_BANDS - 1) * 0.5f;
    const float vblend = 0.25f;

    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        float norm_dist = fabsf((float)i - mid) / mid;
        float hblend = MAX_HBLEND * (norm_dist * norm_dist);

        /* Bottom row color */
        s_bot_lut[i].r = (uint8_t)(base_r + (255 - base_r) * hblend);
        s_bot_lut[i].g = (uint8_t)(base_g + (255 - base_g) * hblend);
        s_bot_lut[i].b = (uint8_t)(base_b + (255 - base_b) * hblend);

        /* Top row color */
        float target_t = 1.0f - hblend / MAX_HBLEND;
        uint8_t trg_r = (uint8_t)(base_r + (255.0f - base_r) * target_t);
        uint8_t trg_g = (uint8_t)(base_g + (255.0f - base_g) * target_t);
        uint8_t trg_b = (uint8_t)(base_b + (255.0f - base_b) * target_t);
        s_top_lut[i].r = (uint8_t)(s_bot_lut[i].r + (trg_r - (float)s_bot_lut[i].r) * vblend);
        s_top_lut[i].g = (uint8_t)(s_bot_lut[i].g + (trg_g - (float)s_bot_lut[i].g) * vblend);
        s_top_lut[i].b = (uint8_t)(s_bot_lut[i].b + (trg_b - (float)s_bot_lut[i].b) * vblend);
    }
}

/* ── Spectrum bar (2 rows, 16-level bars, gradient) ── */
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

    init_gradient_lut();

    /* Read latest processed data from shared buffer (no event overhead) */
    float bands_local[SPECTRUM_BANDS];
    spectrum_get_latest(bands_local);

    int total_w = s.song_panel_width + 29;
    if (total_w < 8) total_w = 8;
    int bands = SPECTRUM_BANDS;
    int cols = (total_w < bands) ? total_w : bands;
    bool dimmed = (s.playback_state != PlaybackState::Playing);

    /* ── Downsample pre-processed spectrum to columns and render ── */
    Elements columns;
    for (int ci = 0; ci < cols; ci++) {
        int start = ci * bands / cols;
        int end = (ci + 1) * bands / cols;
        if (end > bands) end = bands;
        int count = end - start;
        if (count <= 0) continue;

        /* Average pre-processed band values (already emph+env+gain, 0~1) */
        float sum = 0.0f;
        for (int j = start; j < end; j++) sum += bands_local[j];
        float v = sum / (float)count;
        if (v > 1.0f) v = 1.0f;

        int i = (start + end) / 2;

        /* Map to 2-row bar */
        int total = (int)(v * 16.0f + 0.5f);
        if (total > 16) total = 16;
        int bot = (total > 8) ? 8 : total;
        int top = (total > 8) ? (total - 8) : 0;

        char top_str[4], bot_str[4];
        int top_len = LEVEL_BYTES[top][0] == 0x20 ? 1 : 3;
        int bot_len = LEVEL_BYTES[bot][0] == 0x20 ? 1 : 3;
        memcpy(top_str, LEVEL_BYTES[top], (size_t)top_len);
        top_str[top_len] = '\0';
        memcpy(bot_str, LEVEL_BYTES[bot], (size_t)bot_len);
        bot_str[bot_len] = '\0';

        Element col_elem;
        if (dimmed) {
            col_elem = vbox({
                text(std::string(top_str)) | dim,
                text(std::string(bot_str)) | dim,
            });
        } else {
            col_elem = vbox({
                color(Color::RGB(s_top_lut[i].r, s_top_lut[i].g, s_top_lut[i].b),
                      text(std::string(top_str))),
                color(Color::RGB(s_bot_lut[i].r, s_bot_lut[i].g, s_bot_lut[i].b),
                      text(std::string(bot_str))),
            });
        }
        columns.push_back(std::move(col_elem));
    }

    return hbox({
        filler(),
        hbox(std::move(columns)),
        filler(),
    }) | size(HEIGHT, EQUAL, 2);
}
