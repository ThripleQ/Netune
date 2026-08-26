#include "ui/components/song_list.h"
#include "ui/components/theme_util.h"
#include "ui/components/spinner.h"
#include "ui/state_store.h"
#include "ui/ui_util.h"
#include "core/term_gfx.h"
#include "compat/wcwidth_compat.h"
#include <ftxui/screen/string.hpp>
#include <string>
#include <cwchar>
#include <cstdio>
#include <algorithm>
using namespace ftxui;

#define MARQUEE_SPEED_MS 130   /* ms per column scrolled */
#define MARQUEE_PAUSE_MS 750   /* ms hold at each end */

/* ── Cover-art list rows (kitty-graphics terminals) ──
   In a native-image terminal each song occupies LIST_COVER_ROWS rows:
   a square cover placeholder on the left (the real image is overlaid by
   the frame hook in app.cpp) with the title and artist stacked to its
   right. The placeholder width keeps the image square: a 2-row tall
   cover needs cols = 2 * cell_h / cell_w to match the cell aspect. */
#define LIST_COVER_ROWS 2
int song_list_cover_cols(void) {
    int c = LIST_COVER_ROWS * cover_cell_height() / cover_cell_width();
    if (c < 4) c = 4;
    return c;
}
#define list_cover_cols() song_list_cover_cols()

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
        int cw = compat_wcwidth(wc); if (cw < 0) cw = 1;
        if (col_run + cw > width - 1) break;
        result.append(text, i, rc); col_run += cw; i += rc;
    }
    result += "\u2026";
    return result;
}

static std::string marquee_text(const std::string &text, int width) {
    /* wall-clock driven so the speed is stable regardless of frame rate */
    static std::string                        last_text;
    static std::chrono::steady_clock::time_point start;
    if (text != last_text) {
        last_text = text;
        start = std::chrono::steady_clock::now();
    }
    if (text.empty() || width <= 0) return text;
    int total_w = string_width(text);
    if (total_w <= width) return text;
    int max_offset = total_w - width;
    if (max_offset < 0) max_offset = 0;
    int cycle = max_offset + (int)(MARQUEE_PAUSE_MS / MARQUEE_SPEED_MS);
    auto now = std::chrono::steady_clock::now();
    long long elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    int pos = (int)((elapsed / MARQUEE_SPEED_MS) % cycle);
    int offset_cols = (pos < max_offset) ? pos : max_offset;
    size_t start_i = 0;
    int col_run = 0;
    std::mbstate_t st = {};
    for (size_t i = 0; i < text.size(); ) {
        wchar_t wc = 0;
        size_t rc = mbrtowc(&wc, text.data() + i, text.size() - i, &st);
        if (rc == 0 || rc == (size_t)-1 || rc == (size_t)-2) break;
        int cw = compat_wcwidth(wc); if (cw < 0) cw = 1;
        if (col_run + cw > offset_cols) break;
        start_i = i + rc; col_run += cw; i += rc;
    }
    std::string result;
    col_run = 0; st = {};
    for (size_t i = start_i; i < text.size(); ) {
        wchar_t wc = 0;
        size_t rc = mbrtowc(&wc, text.data() + i, text.size() - i, &st);
        if (rc == 0 || rc == (size_t)-1 || rc == (size_t)-2) break;
        int cw = compat_wcwidth(wc); if (cw < 0) cw = 1;
        if (col_run + cw > width) break;
        result.append(text, i, rc); col_run += cw; i += rc;
    }
    if (result.empty() || string_width(result) == 0) {
        col_run = 0; st = {};
        for (size_t i = 0; i < text.size(); ) {
            wchar_t wc = 0;
            size_t rc = mbrtowc(&wc, text.data() + i, text.size() - i, &st);
            if (rc == 0 || rc == (size_t)-1 || rc == (size_t)-2) break;
            int cw = compat_wcwidth(wc); if (cw < 0) cw = 1;
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
    int avail_w = mw - 5;  /* minus prefix (marker + spaces) */
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
            els.push_back(render_spinner(s));

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
                bool sel = ((int)i == s.selected_index && s.active_panel == 1
                            && !s.top_search_active);
                std::string content;
                if (song.title) content += song.title;
                if (song.artist) content += std::string(" — ") + song.artist;
                std::string row = build_info_row(content, avail_w, sel);
                if (sel) {
                    if (s.action_sheet_open)
                        els.push_back(hbox({theme_sel_marker(false, song.fee == 1),
                                            theme_fg(text(row))}) | focus);
                    else
                        els.push_back(theme_selection(text("> " + row) | focus));
                } else
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
                std::string prefix;
                if (song.fee == 1) prefix = "\xEF\xBC\x84 ";  /* ＄ full-width */
                std::string label;
                if (song.title) label += prefix + song.title;
                if (song.artist) { label += " \u2014 "; label += song.artist; }
                label = fit_text(label, avail_w);
                if (selected) {
                    if (s.action_sheet_open)
                        els.push_back(hbox({theme_sel_marker(false, song.fee == 1),
                                            theme_fg(text(label))}) | focus);
                    else
                        els.push_back(theme_selection(text("> " + label) | focus));
                } else
                    els.push_back(theme_fg(text("  " + label)));
                shown++;
            }
        }

    } else {
        /* ── Normal playlist display ─────────────────── */

        /* top-right search box filters the current list — filter mode
           only (the API-search box never filters: results ARE the API
           response, and its query is kept for memory) */
        const std::string filter_q = s.top_search_api ? "" : s.top_right_query;

        /* the app's own DOWNLOADS group: its list is a merged view of the
           live download tasks (newest first) + the downloaded files. Only
           applies once the group is actually entered (active_panel==1) —
           while merely hovering the left-panel entry the right panel must
           keep its previous content (or the logo), not blank out. */
        bool in_downloads = (s.music_mode == MusicMode::Local &&
                             s.active_panel == 1 &&
                             s.group_index >= 0 &&
                             s.group_index < (int)s.groups.size() &&
                             s.groups[s.group_index].is_downloads);

        /* Spinner during async load (overlaid on top of list below) */

        /* Empty-list hints (netease mode) */
        if (!s.loading && s.playlist.empty() && s.music_mode == MusicMode::Netease) {
            if (!filter_q.empty() && s.top_search_api) {
                els.push_back(theme_fg(text("  按 [Enter] 搜索网易云: " + filter_q)) | dim);
            }
        }

                                /* Watermark: show netease logo when empty */
        if (!s.loading && s.playlist.empty() && !in_downloads) {
            /* Netease logo: half-block chars (▀▄█), 46x18 (equiv 46x36 px).
               Fits standard 80x24 terminal. Lanczos + threshold 80.
               Leading spaces are part of the shape (circle outline).
               Centered via FTXUI center decorator. */
            static const char* logo[] = {
                "                         ▄▄████████▄▄▄",
                "              ▄▄▄▄     ▄███████████████",
                "         ▄▄▄███████    ██████▀   ▀▀▀█▀▀",
                "      ▄▄████████▀▀▀    ██████",
                "    ▄███████▀▀        ▄▄███████▄▄▄▄",
                "  ▄██████▀        ▄███████████████████▄▄",
                " ▄██████       ▄████████████████▀████████▄",
                "▄█████▀       ▄██████▀    ██████    ▀██████▄",
                "██████        ██████      ▀██████     ▀██████",
                "██████        ██████       ██████       ██████",
                "██████        ███████▄   ▄▄██████       ██████",
                "▀█████▄        ▀███████████████▀        ██████",
                " ▀█████▄         ▀▀▀███████▀▀▀         ▄█████▀",
                "  ▀██████▄                           ▄██████▀",
                "    ▀██████▄▄                     ▄▄██████▀",
                "      ▀████████▄▄▄▄         ▄▄▄▄████████▀",
                "         ▀▀██████████████████████████▀▀",
                "             ▀▀▀▀██████████████▀▀▀"
            };
            const int logo_rows = (int)(sizeof(logo)/sizeof(logo[0]));

            Elements logo_els;
            for (int i = 0; i < logo_rows; i++)
                logo_els.push_back(theme_logo(text(logo[i])));

            /* Center horizontally (adapts to actual panel width) and vertically */
            els.push_back(filler());
            els.push_back(vbox(std::move(logo_els)) | center);
            els.push_back(filler());
        }

                int shown_rows = 0;
        for (size_t i = 0; i < s.playlist.size(); i++) {
            const auto &song = s.playlist[i];
            if (!filter_q.empty()) {
                std::string haystack;
                if (song.title) haystack += song.title;
                if (song.artist) haystack += std::string(" ") + song.artist;
                if (!str_icontains(haystack, filter_q)) continue;
            }
            shown_rows++;
            bool sel = ((int)i == s.selected_index);

            std::string title = (song.title && song.title[0]) ? song.title : "(unknown)";

            /* Text badge at the row tail: 歌单 → playlist, VIP → fee==1.
               A playlist row and a VIP single never co-occur. */
            bool is_pl = song.is_playlist;
            bool is_vip = (!is_pl && song.fee == 1);
            std::string tail = is_pl ? " \u6B4C\u5355" : (is_vip ? " VIP" : "");

            /* ── Image-terminal cover rows ────────────────
               Each song spans LIST_COVER_ROWS rows: the square cover
               placeholder column on the left (image overlaid by the frame
               hook in app.cpp), then title / artist stacked to its right.
               Selected rows get a leading ▶ marker and accent text; no
               full-row background — the highlight would fight the overlaid
               artwork. */
            if (term_gfx_active() && !in_downloads && !s.action_sheet_open) {
                bool active_sel = (sel && s.active_panel == 1 && !s.top_search_active);
                std::string pad = active_sel ? "\u25B6 " : "  ";
                std::string cover_ph((size_t)list_cover_cols(), ' ');
                int txt_w = avail_w - list_cover_cols() - 2;
                if (txt_w < 5) txt_w = 5;
                Element cover_el = text(cover_ph);
                std::string t1 = fit_text(title, txt_w);
                std::string a1 = (song.artist && song.artist[0])
                               ? song.artist : "";
                if (!a1.empty()) a1 = fit_text(a1, txt_w);
                std::string line1 = t1;
                if (!a1.empty() && (int)string_width(t1) < txt_w - 4)
                    line1 += std::string(" \u2014 ") + a1;
                Element title_el = text(line1);
                Element artist_el = theme_artist(text(a1));
                if (active_sel) {
                    title_el = title_el | bold | theme_accent;
                    artist_el = artist_el | theme_accent;
                }
                /* first row: title (+ tail badge) */
                Element r1 = hbox({cover_el, text(pad), title_el});
                if (!tail.empty())
                    r1 = hbox({r1, text(tail) | (is_pl ? theme_playlist
                                                       : theme_vip)});
                els.push_back(r1 | focus);
                /* second row: artist (dim, indented under the title) */
                Element r2 = hbox({text(std::string((size_t)list_cover_cols(), ' ')),
                                   text("  "), artist_el});
                els.push_back(r2);
                continue;
            }

            Element line = theme_fg(text(title));
            int line_w = avail_w;
            if (s.action_sheet_open) line_w -= 2;  /* "> " marker block width */
            bool scroll = (s.active_panel == 1 && sel && !s.top_search_active);
            if (scroll) {
                std::string content = title;
                if (song.artist && song.artist[0])
                    content += std::string(" \u2014 ") + song.artist;
                content += tail;
                line = text(build_info_row(content, line_w, true));
            } else {
                if (song.artist && song.artist[0])
                    line = hbox({line, text(" \u2014 ") | theme_fg,
                                 theme_artist(text(song.artist))});
                if (!tail.empty())
                    line = hbox({line, text(tail) | (is_pl ? theme_playlist
                                                           : theme_vip)});
            }

            bool active_sel = (sel && s.active_panel == 1 && !s.top_search_active);
            std::string pad = active_sel ? "> " : "  ";
            if (active_sel && s.action_sheet_open) {
                /* A modal is open on top: keep the selection marker but
                   collapse it to a small colour block at the ">" position
                   instead of a full-row background — the full-row highlight
                   would bleed into the popup's area through dbox's
                   background blending. The block keeps the row's dedicated
                   colour (playlist / VIP) when the theme defines one. */
                els.push_back(hbox({theme_sel_marker(is_pl, is_vip), line}) | focus);
            } else if (active_sel) {
                /* selected: full marker background accent, or the
                   generic selection color for plain rows. The text color
                   is the inverse of the row background so it stays
                   readable on the highlight. */
                if (is_pl) {
                    const auto &th = ThemeManager::instance().current();
                    line = theme_playlist_sel_bg(line);
                    line = line | color(theme_selection_text(
                        th.playlist.has_color ? th.playlist : th.accent));
                } else if (is_vip) {
                    const auto &th = ThemeManager::instance().current();
                    line = theme_vip_sel_bg(line);
                    line = line | color(theme_selection_text(
                        th.vip.has_color ? th.vip : th.warning));
                } else {
                    const auto &th = ThemeManager::instance().current();
                    ThemeColor bg = th.accent_bg.has_color ? th.accent_bg
                                  : (th.accent.has_color ? th.accent
                                                         : ThemeColor{80, 80, 80, true});
                    line = line | color(theme_selection_text(bg));
                    line = line | bgcolor(Color::RGB(bg.r, bg.g, bg.b));
                }
                els.push_back(hbox({text(pad) | bold, line}) | focus);
            } else if (sel) {
                els.push_back(hbox({text(pad) | bold, line}) | focus);
            } else {
                els.push_back(hbox({text(pad), line}));
            }
        }
        if (shown_rows == 0 && !filter_q.empty())
            els.push_back(theme_fg(text("  无匹配")) | dim);

        if (!filter_q.empty() || s.search_active) {
            /* filtered/search rows map 1:1 to visible lines — keep the
               FTXUI frame auto-scroll for those modes */
            auto list = theme_border(theme_bg(vbox(std::move(els)) | vscroll_indicator | yframe | flex | border));
            if (s.loading)
                return dbox({list, render_spinner(s) | center});
            return list;
        }

        /* ── DOWNLOADS group: unified list (tasks + files) ──
           The app's own DOWNLOADS group renders its own list instead of
           the generic one: live download tasks (downloading + queued) as
           ordinary rows on top (newest first), then the downloaded files
           below. Task rows are informational (not selectable); selection
           covers the file rows only, so task_n is folded into the visual
           row offset. */
        int task_n = 0;
        if (in_downloads) {
            /* newest download task first (s.downloads is enqueue order,
               oldest first — iterate in reverse so the newest lands on top) */
            for (auto rit = s.downloads.rbegin(); rit != s.downloads.rend(); ++rit) {
                const auto &it = *rit;
                std::string label = it.title;
                if (!it.artist.empty())
                    label += std::string(" \u2014 ") + it.artist;
                bool dling = (it.status == DlStatus::Downloading);
                std::string suffix;
                if (dling) {
                    int p = it.total > 0 ? (int)(it.done * 100 / it.total) : 0;
                    if (p > 100) p = 100;
                    char b[16];
                    snprintf(b, sizeof b, " %d%%", p);
                    suffix = std::string(" ") + spinner_glyph() + b;
                } else {
                    suffix = " \u6392\u961F";  /* 排队 */
                }
                Element line = hbox({
                    text("  "),
                    theme_fg(text(label)),
                    filler(),
                    dling ? theme_fg(text(suffix))
                          : (theme_fg(text(suffix)) | dim),
                });
                els.insert(els.begin() + task_n, line);
                task_n++;
            }
        }

        /* unfiltered list: manual viewport so mouse clicks can map to
           rows. Clamp the offset to keep the selection visible, then
           slice — content never exceeds the panel, so the frame
           auto-scroll (which would fight the offset) stays inert. */
        const int h = s.screen_height - 5;  /* top 1 + status 2 + borders 2 */
        int n = (int)els.size();
        int off = s.song_list_offset;
        /* visual row of the selected file (each song = LIST_COVER_ROWS
           rows in image terminals, 1 elsewhere) */
        int rows_song = term_gfx_active() ? LIST_COVER_ROWS : 1;
        int sel = task_n + s.selected_index * rows_song;
        if (sel < 0) sel = 0;
        if (sel >= off + h) off = sel - h + rows_song;
        if (sel < off) off = sel;
        if (off > n - h) off = n - h;
        if (off < 0) off = 0;
        if (off != s.song_list_offset)
            StateStore::instance().set_song_list_offset(off);
        Elements vis;
        for (int i = off; i < off + h && i < n; i++)
            vis.push_back(els[i]);
        auto list = theme_border(theme_bg(vbox(std::move(vis)) | vscroll_indicator | yframe | flex | border));
        if (s.loading) {
            return dbox({list, render_spinner(s) | center});
        }
        return list;
    }

    auto list = theme_border(theme_bg(vbox(std::move(els)) | vscroll_indicator | yframe | flex | border));
    if (s.loading) {
        /* Overlay spinner on top of list so it's visible even when scrolled */
        return dbox({list, render_spinner(s) | center});
    }
    return list;
}
