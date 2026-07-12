#include "ui/components/song_list.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <cwchar>
#include <algorithm>
using namespace ftxui;

#define MARQUEE_SPEED  8
#define MARQUEE_PAUSE 45

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

Element render_song_list(const AppState &s) {
    Elements els;
    int mw = s.song_panel_width;
    if (mw < 15) mw = 15;

    for (size_t i = 0; i < s.playlist.size(); i++) {
        /* format: Title — Artist   03:45 */
        std::string label;
        if (s.playlist[i].title && s.playlist[i].title[0])
            label = s.playlist[i].title;
        else
            label = "(unknown)";
        if (s.playlist[i].artist && s.playlist[i].artist[0])
            label += std::string(" \u2014 ") + s.playlist[i].artist;
        if (s.playlist[i].duration_sec > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "  %02d:%02d",
                     s.playlist[i].duration_sec / 60,
                     s.playlist[i].duration_sec % 60);
            label += buf;
        }
        bool sel = ((int)i == s.selected_index);

        if (s.active_panel == 1 && sel) {
            std::string scrolled = marquee_text(label, mw);
            if (!scrolled.empty()) label = scrolled;
        }

        float fy = s.playlist.size() > 1 ? (float)i / (float)(s.playlist.size() - 1) : 0.0f;
        if (s.active_panel == 1 && sel)
            els.push_back(theme_accent(text("> " + label) | inverted | focusPositionRelative(0, fy)));
        else if (s.active_panel == 0 && sel)
            els.push_back(theme_fg(text("  " + label) | focusPositionRelative(0, fy)));
        else
            els.push_back(theme_fg(text("  " + label)));
    }
    return theme_bg(vbox(std::move(els)) | frame | flex | border);
}
