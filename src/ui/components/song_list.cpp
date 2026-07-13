#include "ui/components/song_list.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <cwchar>
#include <cstdio>
#include <algorithm>
using namespace ftxui;

/* ── Inline spinner (Braille animation) ────────────── */
static Element inline_spinner(bool active) {
    static int frame = 0;
    static bool was_active = false;
    if (!active) { was_active = false; return text(""); }
    if (!was_active) { frame = 0; was_active = true; }
    frame++;
    const char *frames[] = {"\u281B", "\u2819", "\u2819", "\u280B",
                           "\u2803", "\u2807", "\u2807", "\u2812"};
    auto &f = frames[(frame % 8)];
    return hbox({
        text(" " + std::string(f) + " "),
        text("Loading...") | dim,
    });
}

#define MARQUEE_SPEED  8
#define MARQUEE_PAUSE 45

/* ── Truncate or marquee-scroll text within width ─── */
static std::string fit_text(const std::string &text, int width) {
    if (width <= 0) return "";
    int total_w = string_width(text);
    if (total_w <= width) return text;

    std::string result;
    int col_run = 0;
    std::mbstate_t st = {};
    for (size_t i = 0; i < text.size(); ) {
        wchar_t wc = 0;
        size_t rc = mbrtowc(&wc, text.data() + i, text.size() - i, &st);
        if (rc == 0 || rc == (size_t)-1 || rc == (size_t)-2) break;
        int cw = wcwidth(wc); if (cw < 0) cw = 1;
        if (col_run + cw > width - 1) break;
        result.append(text, i, rc); col_run += cw; i += rc;
    }
    result += "\u2026";
    return result;
}

static std::string marquee_text(const std::string &text, int width) {
    static int         frame = 0;
    static std::string last_text;
    if (text != last_text) { frame = 0; last_text = text; }
    frame++;
    if (text.empty() || width <= 0) return text;
    int total_w = string_width(text);
    if (total_w <= width) return text;
    int max_offset = total_w - width;
    if (max_offset < 0) max_offset = 0;
    int cycle = max_offset + MARQUEE_PAUSE;
    int pos = (frame / MARQUEE_SPEED) % cycle;
    int offset_cols = (pos < max_offset) ? pos : max_offset;
    size_t start_i = 0;
    int col_run = 0;
    std::mbstate_t st = {};
    for (size_t i = 0; i < text.size(); ) {
        wchar_t wc = 0;
        size_t rc = mbrtowc(&wc, text.data() + i, text.size() - i, &st);
        if (rc == 0 || rc == (size_t)-1 || rc == (size_t)-2) break;
        int cw = wcwidth(wc); if (cw < 0) cw = 1;
        if (col_run + cw > offset_cols) break;
        start_i = i + rc; col_run += cw; i += rc;
    }
    std::string result;
    col_run = 0; st = {};
    for (size_t i = start_i; i < text.size(); ) {
        wchar_t wc = 0;
        size_t rc = mbrtowc(&wc, text.data() + i, text.size() - i, &st);
        if (rc == 0 || rc == (size_t)-1 || rc == (size_t)-2) break;
        int cw = wcwidth(wc); if (cw < 0) cw = 1;
        if (col_run + cw > width) break;
        result.append(text, i, rc); col_run += cw; i += rc;
    }
    if (result.empty() || string_width(result) == 0) {
        col_run = 0; st = {};
        for (size_t i = 0; i < text.size(); ) {
            wchar_t wc = 0;
            size_t rc = mbrtowc(&wc, text.data() + i, text.size() - i, &st);
            if (rc == 0 || rc == (size_t)-1 || rc == (size_t)-2) break;
            int cw = wcwidth(wc); if (cw < 0) cw = 1;
            if (col_run + cw > width) break;
            result.append(text, i, rc); col_run += cw; i += rc;
        }
    }
    int rw = string_width(result);
    if (rw < width) result.append((size_t)(width - rw), ' ');
    return result;
}

/* ── Build row: Title — Artist, truncated or marquee ── */
static std::string build_info_row(const std::string &content, int avail_w, bool marquee) {
    if (marquee)
        return marquee_text(content, avail_w);
    return fit_text(content, avail_w);
}

/* ══════════════════════════════════════════════════
   Render: either search UI or normal song list
   ══════════════════════════════════════════════════ */
Element render_song_list(const AppState &s) {
    int mw = s.song_panel_width;
    if (mw < 10) mw = 10;
    int avail_w = mw - 2;  /* minus prefix */
    if (avail_w < 5) avail_w = 5;

    Elements els;

    if (s.search_active) {
        /* ── Search input line ──────────────────────── */
        const char *tag = s.search_scope == 0 ? "Filter" :
                          (s.music_mode == MusicMode::Netease ? "Netease" : "Local");
        std::string input = std::string(" [/] ") + tag + " > " + s.search_query + "\u258C";
        els.push_back(text(input) | bold);

        /* ── Search hints per scope ──────────────────── */
        if (s.search_query.empty()) {
            els.push_back(theme_fg(text("  Type to search...")) | dim);
        } else if (s.music_mode == MusicMode::Netease && s.search_results.empty() && !s.loading) {
            els.push_back(theme_fg(text("  Press [Enter] to search Netease")) | dim);
        } else if (s.music_mode != MusicMode::Netease && s.search_results.empty() && !s.loading) {
            els.push_back(theme_fg(text("  No results.")) | dim);
        }

        /* Inline loading spinner during netease search */
        if (s.loading)
            els.push_back(inline_spinner(true));

        /* ── scope=0 (filter): show filtered playlist ─── */
        if (s.search_scope == 0 && !s.search_query.empty()) {
            std::string q = s.search_query;
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            int shown = 0;
            for (size_t i = 0; i < s.playlist.size(); i++) {
                const auto &song = s.playlist[i];
                std::string haystack;
                if (song.title) haystack += song.title;
                if (song.artist) haystack += std::string(" ") + song.artist;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
                if (haystack.find(q) == std::string::npos) continue;
                bool sel = ((int)i == s.selected_index && s.active_panel == 1);
                std::string content;
                if (song.title) content += song.title;
                if (song.artist) content += std::string(" — ") + song.artist;
                std::string row = build_info_row(content, avail_w, sel);
                if (sel)
                    els.push_back(theme_accent(text("> " + row) | inverted | focus));
                else
                    els.push_back(theme_fg(text("  " + row)));
                shown++;
            }
            if (shown == 0)
                els.push_back(theme_fg(text("  No matches.")) | dim);
        }

        /* ── Local search results inline (scope=1) ──── */
        if (s.search_scope == 1 && !s.search_results.empty()) {
            char hdr[32];
            snprintf(hdr, sizeof(hdr), "  %d/%d results:",
                     (int)s.search_results.size(), s.search_total);
            els.push_back(theme_accent(text(hdr) | bold));
            int shown = 0;
            for (auto &song : s.search_results) {
                if (shown >= 30) break;
                bool selected = (shown == s.search_selected);
                std::string label;
                if (song.title) label += song.title;
                if (song.artist) { label += " \u2014 "; label += song.artist; }
                label = fit_text(label, avail_w);
                if (selected)
                    els.push_back(theme_accent(text("> " + label) | inverted | focus));
                else
                    els.push_back(theme_fg(text("  " + label)));
                shown++;
            }
        }

    } else {
        /* ── Normal playlist display ─────────────────── */

        /* Spinner during async load */
        if (s.loading && s.playlist.empty()) {
            els.push_back(filler());
            els.push_back(inline_spinner(true) | center);
            els.push_back(filler());
        } else if (s.loading) {
            els.push_back(inline_spinner(true));
        }

                                /* Watermark: show netease logo when empty */
        if (!s.loading && s.playlist.empty()) {
            /* Pixel data: 40x20, RGB interleaved, -1=transparent (dimmed 35%, clear_under) */
            static const int wp[] = {

     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  85,  85,  86,  84,  84,
     86,  84,  84,  86,  84,  84,  86,  85,  85,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  85,  85,
     86,  85,  85,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  87,  87,  86,  89,  89,  86,  89,  89,  86,  89,  89,  86,  89,  89,  86,  89,  89,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  85,  85,  86,  86,  86,  86,  89,  89,  86,  89,  89,  86,  89,  89,  86,  86,  86,  86,  85,  85,  86,  89,  89,  85,  79,  79,  83,  57,  58,  82,  43,  44,
     82,  41,  42,  82,  49,  50,  84,  66,  66,  85,  84,  84,  86,  86,  86,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  85,  85,  86,  89,  89,  86,  89,  89,  85,  85,  85,  84,  72,  72,
     83,  56,  57,  83,  58,  58,  85,  82,  82,  86,  87,  87,  82,  52,  53,  79,   9,  10,  79,   1,   3,  79,   6,   8,  79,  10,  11,  79,   4,   6,  78,   0,   0,  80,  19,  20,  85,  81,  81,  86,  87,  87,  86,  85,  85,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  85,  85,  86,  87,  87,  86,  89,  89,  84,  71,  71,  81,  39,  40,  79,  11,  13,  79,   0,   2,  79,   2,   3,  79,  15,  17,  85,  76,  76,  86,  89,  89,  79,  17,  19,  79,   0,   1,  80,  18,  19,  85,  86,  85,
     86,  89,  89,  85,  84,  84,  84,  64,  64,  84,  66,  67,  86,  84,  84,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  88,  88,  85,  79,  80,  81,  34,  35,  79,   2,   3,  78,   0,   0,  80,  24,  26,  83,  56,  56,
     85,  76,  77,  86,  89,  89,  86,  88,  88,  85,  80,  80,  81,  33,  34,  79,   3,   5,  79,   7,   8,  81,  37,  38,  83,  58,  58,  84,  68,  68,  85,  85,  85,  86,  89,  89,  86,  89,  89,  86,  86,  86,  86,  85,  85,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,
     86,  88,  88,  84,  73,  73,  79,  14,  15,  79,   0,   0,  80,  16,  18,  84,  66,  67,  86,  89,  89,  86,  89,  89,  86,  88,  88,  82,  52,  53,  80,  22,  23,  79,   3,   5,  79,   2,   4,  79,   4,   7,  79,   6,   8,  79,   1,   3,
     79,   4,   6,  79,   0,   2,  79,   3,   5,  80,  24,  25,  83,  55,  56,  85,  85,  85,  86,  89,  89,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  86,  86,  85,  83,  83,  79,  17,  18,  79,   0,   1,  80,  16,  18,  85,  79,  79,  86,  89,  89,  86,  85,  85,  85,  81,  81,
     80,  24,  25,  79,   0,   1,  79,   2,   3,  81,  33,  35,  84,  66,  66,  81,  38,  39,  79,   1,   3,  79,   4,   5,  82,  49,  49,  84,  71,  71,  81,  41,  42,  79,   7,   9,  79,   0,   0,  80,  18,  19,  84,  69,  70,  86,  89,  89,
     86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  84,  85,  86,  89,  89,
     82,  55,  56,  79,   1,   3,  79,   2,   4,  82,  54,  54,  86,  89,  89,  85,  82,  82,  86,  89,  89,  82,  47,  48,  79,   0,   1,  79,   4,   5,  82,  52,  53,  86,  89,  89,  86,  89,  89,  85,  83,  82,  79,  15,  16,  79,   3,   4,
     80,  16,  17,  85,  83,  83,  86,  89,  89,  85,  79,  80,  80,  23,  24,  79,   0,   1,  79,   8,  10,  84,  73,  73,  86,  87,  87,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  84,  84,  86,  89,  89,  82,  47,  48,  79,   2,   3,  79,   3,   4,  83,  61,  62,  86,  88,  88,  85,  83,  83,  86,  89,  89,  82,  52,  53,
     79,   0,   1,  79,   3,   4,  81,  42,  43,  86,  86,  86,  86,  87,  87,  85,  84,  84,  80,  25,  27,  79,   2,   3,  79,   1,   3,  84,  69,  70,  86,  87,  87,  86,  89,  89,  84,  68,  68,  79,   5,   7,  79,   1,   2,  81,  37,  38,
     86,  89,  89,  86,  84,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  88,  88,
     84,  70,  70,  79,   4,   5,  79,   2,   4,  80,  32,  33,  86,  89,  89,  86,  85,  85,  86,  85,  85,  86,  86,  86,  81,  40,  41,  79,   5,   7,  79,   0,   0,  79,  10,  12,  80,  18,  20,  79,   6,   8,  79,   0,   1,  79,   8,  10,
     82,  50,  51,  86,  87,  87,  85,  83,  83,  86,  89,  89,  84,  71,  71,  79,   6,   8,  79,   1,   3,  81,  37,  37,  86,  89,  89,  86,  84,  84,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  89,  89,  81,  44,  45,  79,   0,   1,  79,   2,   4,  82,  47,  48,  86,  89,  89,  86,  89,  89,  86,  86,  86,
     86,  89,  88,  84,  72,  73,  82,  51,  52,  81,  36,  37,  81,  32,  33,  81,  39,  40,  83,  55,  56,  85,  77,  77,  86,  89,  89,  86,  86,  86,  86,  89,  89,  85,  86,  86,  80,  29,  30,  79,   1,   2,  79,   5,   7,  84,  71,  71,
     86,  88,  88,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  85,  85,  86,  88,  88,  82,  48,  49,  79,   5,   7,  79,   0,   0,  80,  26,  28,  84,  67,  68,  86,  87,  87,  86,  89,  89,  86,  89,  89,  87,  89,  89,  86,  89,  89,  86,  89,  89,  86,  89,  89,  87,  89,  89,  86,  89,  89,
     86,  89,  89,  86,  86,  86,  84,  64,  64,  80,  20,  21,  79,   0,   0,  79,   9,  10,  83,  61,  62,  86,  89,  89,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  89,  89,  84,  72,  72,  81,  34,  35,  79,   4,   5,  78,   0,   0,  80,  14,  16,
     81,  36,  37,  82,  52,  53,  84,  63,  63,  84,  68,  68,  84,  70,  70,  84,  67,  68,  84,  61,  61,  82,  50,  51,  81,  33,  34,  79,  12,  13,  78,   0,   0,  79,   6,   7,  81,  36,  37,  85,  76,  76,  86,  89,  89,  86,  85,  85,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  89,  88,  86,  89,  89,  85,  77,  78,  83,  54,  55,  81,  30,  31,  80,  13,  15,  79,   5,   7,  79,   3,   4,  79,   3,   4,  79,   3,   4,  79,   3,   4,  79,   3,   5,  79,   6,   8,
     80,  15,  16,  81,  32,  33,  83,  56,  57,  85,  78,  78,  86,  89,  89,  86,  88,  88,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  85,  85,  86,  88,  88,  86,  89,  89,  86,  89,  89,
     86,  87,  87,  85,  80,  80,  84,  74,  74,  84,  70,  70,  84,  69,  69,  84,  71,  71,  85,  75,  75,  85,  82,  82,  86,  88,  88,  86,  89,  89,  86,  89,  89,  86,  88,  87,  86,  85,  85,  86,  85,  85,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  86,  84,  85,  86,  85,  85,  86,  86,  86,  86,  87,  87,  86,  88,  88,  86,  89,  89,  86,  89,  89,  86,  89,  89,  86,  88,  88,  86,  87,  87,
     86,  86,  86,  86,  85,  85,  86,  85,  85,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  85,  85,  86,  85,  85,  86,  85,  85,  86,  85,  85,  86,  85,  85,  86,  85,  85,  86,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,
     86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86,  86
            };
            int ip = 0;
            Elements rows;
            for (int r = 0; r < 20; r++) {
                Elements cells;
                for (int c = 0; c < 40; c++) {
                    int rr = wp[ip++], gg = wp[ip++], bb = wp[ip++];
                    Element ch = (rr >= 0)
                        ? color(Color::RGB(rr,gg,bb), text("\u2588"))
                        : text(" ");
                    cells.push_back(ch | clear_under);
                }
                rows.push_back(hbox(std::move(cells)));
            }
            els.push_back(filler());
            els.push_back(vbox(std::move(rows)) | center);
            els.push_back(filler());
        }

                for (size_t i = 0; i < s.playlist.size(); i++) {
            const auto &song = s.playlist[i];
            bool sel = ((int)i == s.selected_index);

            std::string content;
            if (song.title && song.title[0])
                content = song.title;
            else
                content = "(unknown)";
            if (song.artist && song.artist[0])
                content += std::string(" \u2014 ") + song.artist;

            bool scroll = (s.active_panel == 1 && sel);
            std::string row = build_info_row(content, avail_w, scroll);

            if (s.active_panel == 1 && sel)
                els.push_back(theme_accent(text("> " + row) | inverted | focus));
            else if (s.active_panel == 0 && sel)
                els.push_back(theme_fg(text("  " + row) | focus));
            else
                els.push_back(theme_fg(text("  " + row)));
        }
    }

    return theme_bg(vbox(std::move(els)) | vscroll_indicator | frame | flex | border);
}
