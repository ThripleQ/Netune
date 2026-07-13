#include "ui/components/song_list.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <cwchar>
#include <cstdio>
#include <algorithm>
using namespace ftxui;

#define MARQUEE_SPEED  8
#define MARQUEE_PAUSE 45

/* ── Truncate or marquee-scroll text within width ─── */
/* Returns (fit part, is_truncated)                       */
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
        if (col_run + cw > width - 1) break; /* leave room for "…" */
        result.append(text, i, rc); col_run += cw; i += rc;
    }
    result += "\u2026"; /* ellipsis */
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
static std::string build_row(const std::string &content,
                             int duration_sec,
                             int panel_width,
                             bool marquee) {
    (void)duration_sec;  /* time shown in progress bar */
    int avail_w = panel_width - 2;  /* "  " or "> " prefix */
    if (avail_w < 5) avail_w = 5;

    if (marquee)
        return marquee_text(content, avail_w);
    return fit_text(content, avail_w);
}

/* ── Render ────────────────────────────────────────── */
Element render_song_list(const AppState &s) {
    Elements els;
    int mw = s.song_panel_width;
    if (mw < 15) mw = 15;

    for (size_t i = 0; i < s.playlist.size(); i++) {
        const auto &song = s.playlist[i];
        bool sel = ((int)i == s.selected_index);

        /* Build content: "Title — Artist" */
        std::string content;
        if (song.title && song.title[0])
            content = song.title;
        else
            content = "(unknown)";
        if (song.artist && song.artist[0])
            content += std::string(" \u2014 ") + song.artist;

        bool scroll = (s.active_panel == 1 && sel);
        std::string row = build_row(content, song.duration_sec, mw, scroll);

        if (s.active_panel == 1 && sel)
            els.push_back(theme_accent(text("> " + row) | inverted | focus));
        else if (s.active_panel == 0 && sel)
            els.push_back(theme_fg(text("  " + row) | focus));
        else
            els.push_back(theme_fg(text("  " + row)));
    }
    return theme_bg(vbox(std::move(els)) | vscroll_indicator | frame | flex | border);
}
