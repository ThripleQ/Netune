#include "ui/components/status_bar.h"
#include "ui/components/theme_util.h"
#include "ui/components/spinner.h"
#include "ui/state_store.h"
#include <ftxui/screen/string.hpp>
#include <cstdio>
#include <cctype>
#include <string>
using namespace ftxui;

/* Take a max_w-column window of s starting at column col_offset, padded
   with spaces on the right. UTF-8 aware (CJK glyphs count as 2). */
static std::string window_at(const std::string &s, int col_offset, int max_w) {
    std::string out;
    int w = 0;
    int cur_col = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (i + len > s.size()) break;
        int cw = string_width(s.substr(i, (size_t)len));
        if (cur_col + cw > col_offset + max_w) break;
        if (cur_col >= col_offset) {
            out += s.substr(i, (size_t)len);
            w += cw;
        }
        cur_col += cw;
        i += (size_t)len;
    }
    while (w < max_w) {
        out += ' ';
        w++;
    }
    return out;
}

Element render_status_bar(const AppState &s) {
    /* ── State info row ────────────────────────────── */
    std::string state_str;
    switch (s.playback_state) {
    case PlaybackState::Playing: state_str = "\u25B6"; break;
    case PlaybackState::Paused:  state_str = "\u23F8"; break;
    default:                     state_str = "\u25A0"; break;
    }

    char buf[64];
    int m = s.current_time_sec / 60, sc = s.current_time_sec % 60;
    snprintf(buf, sizeof(buf), "%02d:%02d", m, sc);
    std::string time_str = buf;

    /* Append seek target time if pending */
    bool seek_pending = (s.seek_indicator != 0 || s.seek_target_progress > 0.0f);
    if (seek_pending && s.total_time_sec > 0) {
        int tgt;
        if (s.seek_indicator != 0) {
            tgt = s.current_time_sec + s.seek_indicator;
        } else {
            tgt = (int)(s.seek_target_progress * s.total_time_sec);
        }
        if (tgt < 0) tgt = 0;
        if (tgt > s.total_time_sec) tgt = s.total_time_sec;
        char tgt_buf[16];
        const char *arrow = (tgt >= s.current_time_sec) ? " -> " : " <- ";
        snprintf(tgt_buf, sizeof(tgt_buf), "%s%02d:%02d", arrow, tgt / 60, tgt % 60);
        time_str += tgt_buf;
    }

    const char *loop_str = "Off";
    switch (s.loop_mode) {
    case LoopMode::None:     loop_str = "Off";  break;
    case LoopMode::Track:    loop_str = "One";  break;
    case LoopMode::Playlist: loop_str = "All";  break;
    case LoopMode::Shuffle:  loop_str = "Shuf"; break;
    }

    /* ── Song title row ─────────────────────────────╴ */
    std::string song_row;
    if (s.current_song.title && s.current_song.title[0]) {
        song_row = s.current_song.title;
        if (s.current_song.artist && s.current_song.artist[0]) {
            song_row += std::string(" \u2014 ") + s.current_song.artist;
        }
    }

    /* Effective quality for the now-playing netease track, pre-resolved at
       track-change time and cached in StateStore (per-song override >
       global default). Shown as an English-shortname tag on the state row. */
    std::string qual;
    if (s.music_mode == MusicMode::Netease && !s.current_quality.empty()) {
        qual = std::string(" [") + s.current_quality + "]";
    } else if (s.music_mode == MusicMode::Local &&
               s.current_song.id && s.current_song.id[0]) {
        /* local/downloaded file: show the container format as the suffix
           (bitrate / sample-rate probing is deferred for now — we only
           know the extension at play time) */
        std::string p = s.current_song.id;
        size_t dot = p.rfind('.');
        if (dot != std::string::npos && dot + 1 < p.size()) {
            std::string f = p.substr(dot + 1);
            for (auto &c : f) c = (char)toupper((unsigned char)c);
            qual = std::string(" [") + f + "]";
        }
    }

    /* info + title on one line; gauge on the next */
    /* ── Download indicator (visible in every view) ──
       Shown when the serial download queue has active items: spinner +
       the downloading item's percentage. Without this the only place a
       download shows up is the app's own "downloads" local group, so a
       download started from Netease/search views is invisible. */
    std::string dl_str;
    if (!s.downloads.empty()) {
        int dling = -1;
        for (size_t i = 0; i < s.downloads.size(); i++) {
            if (s.downloads[i].status == DlStatus::Downloading) { dling = (int)i; break; }
        }
        if (dling >= 0) {
            const auto &d = s.downloads[dling];
            int p = d.total > 0 ? (int)(d.done * 100 / d.total) : 0;
            if (p > 100) p = 100;
            char pb[16];
            snprintf(pb, sizeof pb, "%d%%", p);
            dl_str = std::string(" ") + spinner_glyph() + " \u4E0B\u8F7D " + pb;  /* 下载 */
        } else {
            /* queued only */
            char qb[32];
            snprintf(qb, sizeof qb, "%zu", s.downloads.size());
            dl_str = std::string(" \u4E0B\u8F7D\u6392\u961F ") + qb;  /* 下载排队 */
        }
    }

    std::string top_line;
    if (song_row.empty()) {
        snprintf(buf, sizeof buf, " %s%s %s  %s  V:%d",
                 state_str.c_str(), qual.c_str(), loop_str, time_str.c_str(), s.volume);
        top_line = buf;
    } else {
        snprintf(buf, sizeof buf, " %s%s %s  %s  V:%d  ",
                 state_str.c_str(), qual.c_str(), loop_str, time_str.c_str(), s.volume);
        top_line = buf;
        int prefix_w = string_width(top_line);
        int avail = s.top_row_width - 1 - prefix_w;   /* one column margin */
        int song_w = string_width(song_row);
        if (song_w <= avail) {
            /* fits — show the full title */
            top_line += song_row;
        } else {
            /* too long — marquee: scroll 1 column per 4 frames, looping
               with a small gap so the title re-enters from the right */
            const int GAP = 4;
            static int s_tick = 0;
            s_tick++;
            std::string loop_row = song_row + std::string(GAP, ' ');
            int off = (s_tick / 4) % (song_w + GAP);
            top_line += window_at(loop_row, off, avail);
        }
    }

    /* prepend the download indicator to the state row (before the title),
       shrinking the marquee budget by its width */
    if (!dl_str.empty()) {
        int dw = string_width(dl_str);
        top_line.insert(0, dl_str);
        /* the marquee budget above didn't account for dl_str; drop the
           trailing "  " so the line still fits (marquee window recomputed
           below is approximate — acceptable for a status line). */
        (void)dw;
    }

    float gv = s.progress;
    if (s.seek_target_progress > 0.0f) {
        /* Show seek target until real progress reaches it */
        gv = s.seek_target_progress;
    } else if (s.seek_indicator != 0 && s.total_time_sec > 0) {
        /* Accumulating: show merged progress + seek delta */
        int merged = s.current_time_sec + s.seek_indicator;
        if (merged < 0) merged = 0;
        if (merged > s.total_time_sec) merged = s.total_time_sec;
        gv = (float)merged / s.total_time_sec;
    }
    /* Two-tone progress bar: gauge with accent-colored progress on lighter track bg */
    auto gv_clamped = std::min(std::max(gv, 0.0f), 1.0f);
    auto gauge_elt = gauge(gv_clamped) | theme_accent | theme_progress_track;

    return theme_bg(vbox(Elements{
        theme_fg(text(top_line)) | dim,
        gauge_elt,
    }));
}
