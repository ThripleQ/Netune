#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/components/spinner.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include "core/spectrum.h"
#include "core/term_gfx.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
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

/* Cover display height cap (keeps the panel sane on tall covers; the
   lyrics panel follows the same height). */
#define COVER_MAX_ROWS 20

/* Aspect-preserving fit of the cover into a `slot_w`-column slot.
   Returns the display size dw×dh that shows the ENTIRE image with no
   distortion and no crop: when the COVER_MAX_ROWS cap kicks in, the
   width is reduced instead of squashing the image vertically. */
static void cover_fit(const CoverData &cd, int slot_w, int *dw_out, int *dh_out) {
    int dw = slot_w, dh = 0;
    if (cd.pixels && cd.width > 0 && cd.height > 0) {
        /* Probed cell aspect: source rows consumed per terminal row for an
           undistorted rendering (2:1 cells → 2, the classic half-block
           case). Keeps the cover square on any terminal font. */
        double step = (double)cover_cell_height() / (double)cover_cell_width();
        if (step < 1.0) step = 1.0;
        if (dw > cd.width) dw = cd.width;   /* never upscale */
        if (dw < 1) dw = 1;
        dh = (int)(cd.height * (double)dw / (double)cd.width / step);
        if (dh < 1) dh = 1;
        if (dh > COVER_MAX_ROWS) {
            dh = COVER_MAX_ROWS;
            dw = (int)(cd.width * (double)dh * step / (double)cd.height);
            if (dw < 1) dw = 1;
        }
    }
    if (dw_out) *dw_out = dw;
    if (dh_out) *dh_out = dh;
}

void cover_layout(const AppState &s, int *cw, int *dw, int *dh) {
    int total = s.song_panel_width + 29;
    int w = total / 2 - 1;
    if (w < 12) w = 12;
    if (w > 60) w = 60;
    if (s.cover.width > 0 && w > s.cover.width) w = s.cover.width;
    int fw = 0, fh = 0;
    cover_fit(s.cover, w, &fw, &fh);
    if (cw) *cw = w;    /* slot width (lyrics layout follows this) */
    if (dw) *dw = fw;   /* actual cover width, centered in the slot */
    if (dh) *dh = fh;   /* actual cover rows (≤ COVER_MAX_ROWS) */
}

/* ── Half-block downsampling ────────────────────────────────
   The full source image is mapped onto dw×dh terminal cells (two
   vertical sub-pixels per cell via ▀). Boundaries are contiguous floors
   over the FULL source extent — every source pixel contributes to
   exactly one output sub-pixel, no gaps, no overlap.

   The old code advanced the source by a cell-aspect "step" (≈2) rows per
   terminal row, which is only correct at 1:1 scale. For any downscaled
   cover it sampled just the top strip of the image (e.g. the top 40 of
   300 rows), so the mosaic showed a horizontal band instead of the cover
   and no outline of the original was recognizable. */

/* Average one source rectangle. Accumulation happens in approximate
   linear light (square law) instead of raw sRGB: plain sRGB averaging
   darkens midtones and smears high-contrast edges — exactly where cover
   art outlines live. */
static void cover_avg_region(const CoverData &cd, int x0, int x1, int y0, int y1,
                             uint8_t out[3]) {
    uint64_t r = 0, g = 0, b = 0;
    size_t n = 0;
    int ch = cd.channels > 0 ? cd.channels : 3;
    for (int sy = y0; sy < y1; sy++) {
        const uint8_t *row = cd.pixels + (size_t)sy * cd.width * ch;
        for (int sx = x0; sx < x1; sx++) {
            const uint8_t *p = row + sx * ch;
            r += (unsigned)p[0] * p[0];
            g += (unsigned)p[1] * p[1];
            b += (unsigned)p[2] * p[2];
            n++;
        }
    }
    if (n == 0) n = 1;
    out[0] = (uint8_t)(sqrtf((float)(r / n)) + 0.5f);
    out[1] = (uint8_t)(sqrtf((float)(g / n)) + 0.5f);
    out[2] = (uint8_t)(sqrtf((float)(b / n)) + 0.5f);
}

/* Resample the whole cover into dw×dh cells. `out` receives
   dw*dh*2*3 bytes: top, then bottom half-block color per cell. */
static void cover_resample(const CoverData &cd, int dw, int dh,
                           std::vector<uint8_t> &out) {
    int sw = cd.width, sh = cd.height;
    out.assign((size_t)dw * dh * 2 * 3, 0);
    const int sub_rows = dh * 2;   /* half-block sub-rows */
    for (int y = 0; y < dh; y++) {
        int t0 = (2 * y) * sh / sub_rows;
        int t1 = (2 * y + 1) * sh / sub_rows;
        int b1 = (2 * y + 2) * sh / sub_rows;
        if (t1 <= t0) t1 = t0 + 1;
        if (b1 <= t1) b1 = t1 + 1;
        if (b1 > sh) b1 = sh;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw;
            int x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            uint8_t *cell = &out[(size_t)(y * dw + x) * 6];
            cover_avg_region(cd, x0, x1, t0, t1, cell);      /* top */
            cover_avg_region(cd, x0, x1, t1, b1, cell + 3);  /* bottom */
        }
    }
}

static Element render_cover(const CoverData &cd, int dw, int dh) {
    if (!cd.pixels || cd.width <= 0 || cd.height <= 0 || dw < 1 || dh < 1)
        return vbox({text("")}) | center | flex;

    /* Terminal supports the kitty graphics protocol: reserve the same
       cell area as blank space — the real image is placed on top by the
       frame hook in app.cpp, keeping the layout identical to the
       character fallback. */
    if (term_gfx_active()) {
        Elements rows;
        std::string blank((size_t)dw, ' ');
        for (int y = 0; y < dh; y++)
            rows.push_back(text(blank));
        return vbox(std::move(rows)) | center | flex;
    }

    /* Downsampled cell colors, cached: the resample touches every source
       pixel, so it only runs when the cover or the target size changed
       (i.e. on window resize / new cover) — per-frame cost is just
       emitting the characters. */
    struct CellCache {
        uint64_t stamp = 0;
        int sw = 0, sh = 0, dw = 0, dh = 0;
        std::vector<uint8_t> rgb;
    };
    static CellCache cache;
    if (cache.stamp != cd.stamp || cache.sw != cd.width ||
        cache.sh != cd.height || cache.dw != dw || cache.dh != dh) {
        cover_resample(cd, dw, dh, cache.rgb);
        cache.stamp = cd.stamp;
        cache.sw = cd.width;
        cache.sh = cd.height;
        cache.dw = dw;
        cache.dh = dh;
    }

    Elements rows;
    for (int y = 0; y < dh; y++) {
        Elements cells;
        for (int x = 0; x < dw; x++) {
            const uint8_t *c = &cache.rgb[(size_t)(y * dw + x) * 6];
            cells.push_back(bgcolor(
                Color::RGB(c[0], c[1], c[2]),
                color(Color::RGB(c[3], c[4], c[5]),
                    text("\u2580"))));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    return vbox(std::move(rows)) | center | flex;
}

Element render_cover_only(const AppState &s) {
    int cw = 0, dw = 0, dh = 0;
    cover_layout(s, &cw, &dw, &dh);

    if (s.cover.pixels && s.cover.width > 0 && s.cover.height > 0 &&
        dw > 0 && dh > 0)
        return render_cover(s.cover, dw, dh) | center | flex;
    if (s.cover_loading)
        return render_spinner(s) | center | flex;
    return vbox({text("")}) | center | flex;
}

/* Lyrics panel height follows the cover's rendered height (they sit side
   by side and share the same vertical extent); falls back to 20 rows
   while no cover is loaded. */
static int cover_panel_height(const AppState &s) {
    int dh = 0;
    cover_layout(s, NULL, NULL, &dh);
    return dh > 0 ? dh : COVER_MAX_ROWS;
}

Element render_lyrics_only(const AppState &s) {
    int total = s.song_panel_width + 29;
    int cw = total / 2 - 1;
    if (cw < 12) cw = 12;
    if (cw > 60) cw = 60;
    int lw = total - cw - 1;
    if (lw < 20) lw = 20;
    int h = cover_panel_height(s);
    return render_lyrics(s.lyrics, s.current_time_ms, lw) |
           size(WIDTH, EQUAL, lw) | size(HEIGHT, EQUAL, h);
}

/* Left margin before the cover: 1/16 of the terminal width */
int cover_left_margin(const AppState &s) {
    int m = (s.song_panel_width + 29) / 16;
    return m < 1 ? 1 : m;
}

Element render_lyric_panel(const AppState &s) {
    int total = s.song_panel_width + 29;
    int cw = total / 2 - 1;
    if (cw < 12) cw = 12;
    if (cw > 60) cw = 60;
    int lw = total - cw - 1;
    if (lw < 20) lw = 20;
    /* The panel itself stays flexed to fill the main layout; the inner
       cover+lyrics block is centered vertically so the overall layout
       (spectrum/status bars pinned to the bottom) is unchanged. A left
       margin of 1/16 terminal width keeps the cover off the screen edge. */
    return theme_bg(hbox(Elements{
        filler() | size(WIDTH, EQUAL, cover_left_margin(s)),
        render_cover_only(s) | size(WIDTH, EQUAL, cw),
        render_lyrics_only(s) | size(WIDTH, EQUAL, lw) | center,
    }) | center);
}

/* ── Spectrum bar (adaptive: 2-4 rows, full terminal width) ── */
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

    /* Adaptive height: one row per ~12 terminal rows, 2-4 rows total.
       Each row adds 8 more levels of resolution. */
    int rows = s.screen_height / 12;
    if (rows < 2) rows = 2;
    if (rows > 4) rows = 4;
    const int MAX_LEVELS = rows * 8;

    int bands = SPECTRUM_BANDS;
    int cols = total_w;

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

    /* ── Frequency-weighted sensitivity + smoothing ───── */
    /* Per-band exponent: low bands use sqrt (x^0.5), smoothly ramping
       to x^0.3 at the high end — high-frequency bands get progressively
       more amplification to compensate their naturally lower energy.
       Smoothing: slight rise (~2 frames) and a slower exponential
       release so bars linger briefly. */
    static float s_smooth[SPECTRUM_BANDS] = {0};
    const float FALL_SPEED = 0.24f;   /* per-frame release toward target */
    const float ATTACK_SPEED = 0.65f; /* slight rise smoothing (~2 frames) */
    const float MASTER_GAIN = 1.25f;  /* overall +25% */
    float processed[SPECTRUM_BANDS];
    for (int i = 0; i < bands; i++) {
        float v = s.spectrum[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        float expo = 0.5f - 0.2f * (float)i / (float)bands;
        float target = powf(v, expo) * MASTER_GAIN;
        if (target > 1.0f) target = 1.0f;
        if (target >= s_smooth[i]) {
            s_smooth[i] += (target - s_smooth[i]) * ATTACK_SPEED;
        } else {
            s_smooth[i] += (target - s_smooth[i]) * FALL_SPEED;
        }
        processed[i] = s_smooth[i];
    }

    /* ── Sample bands → columns (average when several bands per
          column, nearest band when more columns than bands) ── */
    Elements columns;
    for (int ci = 0; ci < cols; ci++) {
        int start = ci * bands / cols;
        int end = (ci + 1) * bands / cols;
        if (end > bands) end = bands;
        int count = end - start;
        if (count < 1) {
            /* wider terminal than bands: repeat the nearest band */
            start = (ci * bands / cols);
            end = start + 1;
            if (start >= bands) start = bands - 1;
            count = 1;
        }

        float sum = 0.0f;
        for (int j = start; j < end; j++) sum += processed[j];
        float v = sum / (float)count;
        if (v > 1.0f) v = 1.0f;

        /* Use center band index for gradient */
        int i = (start + end) / 2;
        if (i >= bands) i = bands - 1;

        /* Map to N-row bar: row r (from bottom) shows the slice
           [8*r, 8*(r+1)) of the total level */
        int total = (int)(v * (float)MAX_LEVELS + 0.5f);
        if (total > MAX_LEVELS) total = MAX_LEVELS;

        Elements col_rows;
        for (int r = rows - 1; r >= 0; r--) {
            int slice = total - r * 8;
            if (slice < 0) slice = 0;
            if (slice > 8) slice = 8;

            const char *glyph = (const char*)LEVEL_BYTES[slice];
            int g_len = (slice == 0) ? 1 : 3;
            std::string gs(glyph, (size_t)g_len);
            bool topmost = (r == rows - 1);
            if (dimmed) {
                col_rows.push_back(text(std::move(gs)) | dim);
            } else if (topmost) {
                col_rows.push_back(
                    color(Color::RGB(s_top[i].r, s_top[i].g, s_top[i].b),
                          text(std::move(gs))));
            } else {
                col_rows.push_back(
                    color(Color::RGB(s_bot[i].r, s_bot[i].g, s_bot[i].b),
                          text(std::move(gs))));
            }
        }
        columns.push_back(vbox(std::move(col_rows)));
    }

    return hbox({
        filler(),
        hbox(std::move(columns)),
        filler(),
    }) | size(HEIGHT, EQUAL, rows);
}
