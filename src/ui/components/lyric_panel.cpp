#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/components/spinner.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include "core/spectrum.h"
#include "core/term_gfx.h"
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
void cover_layout(const AppState &s, int *cw, int *dh) {
    int total = s.song_panel_width + 29;
    int w = total / 2 - 1;
    if (w < 12) w = 12;
    if (w > 60) w = 60;
    if (s.cover.width > 0 && w > s.cover.width) w = s.cover.width;
    int h = 0;
    if (s.cover.pixels && s.cover.width > 0 && s.cover.height > 0) {
        /* rows = source pixel rows / step, where step = cell_h/cell_w
           (source rows consumed per terminal row) */
        double step = (double)cover_cell_height() / cover_cell_width();
        if (step < 1.0) step = 1.0;
        h = (int)(s.cover.height * w / (double)s.cover.width / step);
        if (h % 2) h++;
    }
    if (h > 20) h = 20;   /* lyrics panel is fixed at 20 rows */
    if (cw) *cw = w;
    if (dh) *dh = h;
}

static Element render_cover(const CoverData &cd, int panel_w) {
    if (!cd.pixels || cd.width <= 0 || cd.height <= 0 || panel_w < 4)
        return vbox({text("")}) | center | flex;

    int dw = panel_w;
    if (dw > cd.width) dw = cd.width;

    /* Source pixel rows consumed per terminal row, from the probed cell
       aspect (2:1 cell → step 2, the classic half-block case). This keeps
       the rendered cover square on any terminal font. */
    double step = (double)cover_cell_height() / cover_cell_width();
    if (step < 1.0) step = 1.0;
    int dh = (int)(cd.height * dw / (double)cd.width / step);
    if (dh < 1) dh = 1;
    if (dh % 2) dh++;

    /* Terminal supports the kitty graphics protocol: emit Unicode
       placeholder cells (U+10EEEE + row/col diacritics, image id in the
       foreground color). The image is then plain text from FTXUI's point
       of view — layout, diffs, resizes and removal all Just Work.
       NOTE: the color must be applied via FTXUI's color() decorator —
       embedding an ESC sequence in the string would make FTXUI count the
       control char in the text width and shift the whole layout. */
    if (term_gfx_active()) {
        term_gfx_ensure(&cd, dw, dh);
        unsigned long id = term_gfx_image_id();
        if (id == 0) {
            Elements rows;
            std::string blank((size_t)dw, ' ');
            for (int y = 0; y < dh; y++)
                rows.push_back(text(blank));
            return vbox(std::move(rows)) | center | flex;
        }
        /* 24-bit color encodes the image id: R=id&0xFF, G=(id>>8)&0xFF,
           B=(id>>16)&0xFF. Diacritics: U+0305+row / U+0305+col (0xCC
           0x85+n in UTF-8). */
        Elements rows;
        for (int y = 0; y < dh; y++) {
            std::string line;
            for (int x = 0; x < dw; x++) {
                line += "\xf4\x8e\xbb\xae";   /* U+10EEEE placeholder */
                line += (char)0xcc;           /* row diacritic U+0305+y */
                line += (char)(0x85 + y);
                line += (char)0xcc;           /* col diacritic U+0305+x */
                line += (char)(0x85 + x);
            }
            rows.push_back(
                text(std::move(line)) |
                color(Color::RGB((int)(id & 0xFF),
                                 (int)((id >> 8) & 0xFF),
                                 (int)((id >> 16) & 0xFF))));
        }
        return vbox(std::move(rows)) | center | flex;
    }

    int sw = cd.width, sh = cd.height;

    /* Average the source region covered by each terminal half-block
       (box filter) instead of nearest-neighbor sampling — crisper result
       when the stored cover is larger than the panel. */
    auto avg = [&](int x0, int x1, int y0, int y1, uint8_t out[3]) {
        long r = 0, g = 0, b = 0, n = 0;
        for (int sy = y0; sy < y1; sy++) {
            const uint8_t *row = cd.pixels + (size_t)sy * sw * 3;
            for (int sx = x0; sx < x1; sx++) {
                const uint8_t *p = row + sx * 3;
                r += p[0]; g += p[1]; b += p[2]; n++;
            }
        }
        if (n == 0) n = 1;
        out[0] = (uint8_t)(r / n);
        out[1] = (uint8_t)(g / n);
        out[2] = (uint8_t)(b / n);
    };

    Elements rows;
    for (int y = 0; y < dh; y++) {
        /* contiguous floor boundaries: row y covers source rows
           [floor(y*step), floor((y+1)*step)) — no gaps, no overlap,
           no cumulative drift (step=2 matches the classic 2:1 layout) */
        double y0f = y * step;
        double y1f = y0f + step;
        int y0 = (int)y0f;
        int y1 = (int)y1f;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        int ymid = y0 + (y1 - y0) / 2;
        if (ymid <= y0) ymid = y0 + 1;

        Elements cells;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw;
            int x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            uint8_t top[3], bot[3];
            avg(x0, x1, y0, ymid, top);
            avg(x0, x1, ymid, y1, bot);
            cells.push_back(bgcolor(
                Color::RGB(top[0], top[1], top[2]),
                color(Color::RGB(bot[0], bot[1], bot[2]),
                    text("\u2580"))));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(rows)) | center | flex;
}

Element render_cover_only(const AppState &s) {
    int cw = 0, dh = 0;
    cover_layout(s, &cw, &dh);
    (void)dh;

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

    /* Full terminal width for this panel view */
    int total_w = s.song_panel_width + 29;
    if (total_w < 8) total_w = 8;

    int bands = SPECTRUM_BANDS;
    int cols = (total_w < bands) ? total_w : bands;

    /* ── Smoothing state & zone attack/release ────────── */
    static float s_height[SPECTRUM_BANDS] = {0};
    static const float ALPHA_UP[4]   = {0.28f, 0.28f, 0.35f, 0.40f};
    static const float ALPHA_DOWN[4] = {0.28f, 0.28f, 0.18f, 0.24f};

    /* ── Gradient LUT ─────────────────────────────────── */
    static struct { uint8_t r, g, b; } s_bot[SPECTRUM_BANDS], s_top[SPECTRUM_BANDS];
    static uint8_t s_last_r = 0, s_last_g = 0, s_last_b = 0;
    const auto &theme = ThemeManager::instance().current();
    uint8_t base_r = theme.spectrum.has_color ? theme.spectrum.r : theme.accent.r;
    uint8_t base_g = theme.spectrum.has_color ? theme.spectrum.g : theme.accent.g;
    uint8_t base_b = theme.spectrum.has_color ? theme.spectrum.b : theme.accent.b;

    if (base_r != s_last_r || base_g != s_last_g || base_b != s_last_b) {
        s_last_r = base_r; s_last_g = base_g; s_last_b = base_b;
        const float MAX_HBLEND = 0.45f;
        float mid = (float)(SPECTRUM_BANDS - 1) * 0.5f;
        const float vblend = 0.25f;
        for (int k = 0; k < SPECTRUM_BANDS; k++) {
            float norm_dist = fabsf((float)k - mid) / mid;
            float hblend = MAX_HBLEND * (norm_dist * norm_dist);
            s_bot[k].r = (uint8_t)(base_r + (255 - base_r) * hblend);
            s_bot[k].g = (uint8_t)(base_g + (255 - base_g) * hblend);
            s_bot[k].b = (uint8_t)(base_b + (255 - base_b) * hblend);
            float target_t = 1.0f - hblend / MAX_HBLEND;
            uint8_t trg_r = (uint8_t)(base_r + (255.0f - base_r) * target_t);
            uint8_t trg_g = (uint8_t)(base_g + (255.0f - base_g) * target_t);
            uint8_t trg_b = (uint8_t)(base_b + (255.0f - base_b) * target_t);
            s_top[k].r = (uint8_t)(s_bot[k].r + (trg_r - (float)s_bot[k].r) * vblend);
            s_top[k].g = (uint8_t)(s_bot[k].g + (trg_g - (float)s_bot[k].g) * vblend);
            s_top[k].b = (uint8_t)(s_bot[k].b + (trg_b - (float)s_bot[k].b) * vblend);
        }
    }

    bool dimmed = (s.playback_state != PlaybackState::Playing);
    const float MAX_HEIGHT = 16.0f;  /* 2 rows × 8 levels each */

    /* ── Per-band: dB → height → zone smoothing ────── */
    float processed[SPECTRUM_BANDS];
    for (int i = 0; i < bands; i++) {
        float v = s.spectrum[i];
        if (v < 0.0f) v = 0.0f;

        /* Magnitude → dB, clamp to [-60, 0] */
        float d = (v > 1e-6f) ? 20.0f * log10f(v) : -60.0f;
        if (d < -60.0f) d = -60.0f;
        if (d > 0.0f) d = 0.0f;

        /* dB → height in [0, 16] */
        float target = (d + 60.0f) / 60.0f * MAX_HEIGHT;
        if (target < 0.0f) target = 0.0f;
        if (target > MAX_HEIGHT) target = MAX_HEIGHT;

        /* Zone-specific attack/release smoothing */
        int zone = (i < 24) ? 0 : (i < 64) ? 1 : (i < 104) ? 2 : 3;
        float a = (target > s_height[i]) ? ALPHA_UP[zone] : ALPHA_DOWN[zone];
        s_height[i] += a * (target - s_height[i]);

        /* Clamp and store as 0~1 ratio for visual mapping */
        if (s_height[i] < 0.0f) s_height[i] = 0.0f;
        processed[i] = s_height[i] / MAX_HEIGHT;
        if (processed[i] > 1.0f) processed[i] = 1.0f;
    }

    /* ── Downsample to columns and render ────────────── */
    Elements columns;
    for (int ci = 0; ci < cols; ci++) {
        int start = ci * bands / cols;
        int end = (ci + 1) * bands / cols;
        if (end > bands) end = bands;
        int count = end - start;
        if (count <= 0) continue;

        float sum = 0.0f;
        for (int j = start; j < end; j++) sum += processed[j];
        float v = sum / (float)count;
        if (v > 1.0f) v = 1.0f;

        /* Use center band index for gradient */
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
                color(Color::RGB(s_top[i].r, s_top[i].g, s_top[i].b),
                      text(std::string(top_str))),
                color(Color::RGB(s_bot[i].r, s_bot[i].g, s_bot[i].b),
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
