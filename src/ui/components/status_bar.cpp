#include "ui/components/status_bar.h"
#include "ui/components/theme_util.h"
#include <cstdio>
#include <string>
#include <algorithm>
using namespace ftxui;

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
    if (s.seek_indicator != 0 && s.total_time_sec > 0) {
        int tgt = s.current_time_sec + s.seek_indicator;
        if (tgt < 0) tgt = 0;
        if (tgt > s.total_time_sec) tgt = s.total_time_sec;
        char tgt_buf[16];
        snprintf(tgt_buf, sizeof(tgt_buf), " -> %02d:%02d", tgt / 60, tgt % 60);
        time_str += tgt_buf;
    }

    const char *loop_str = "Off";
    switch (s.loop_mode) {
    case LoopMode::None:     loop_str = "Off";  break;
    case LoopMode::Track:    loop_str = "One";  break;
    case LoopMode::Playlist: loop_str = "All";  break;
    }

    /* ── Song title row ─────────────────────────────╴ */
    std::string song_row;
    if (s.current_song.title && s.current_song.title[0]) {
        song_row = s.current_song.title;
        if (s.current_song.artist && s.current_song.artist[0]) {
            song_row += std::string(" \u2014 ") + s.current_song.artist;
        }
    }

    /* info + title on one line; gauge on the next */
    std::string top_line;
    if (song_row.empty()) {
        snprintf(buf, sizeof(buf), " %s  %s  %s  V:%d",
                 state_str.c_str(), loop_str, time_str.c_str(), s.volume);
        top_line = buf;
    } else {
        snprintf(buf, sizeof(buf), " %s  %s  %s  V:%d  %s",
                 state_str.c_str(), loop_str, time_str.c_str(),
                 s.volume, song_row.c_str());
        top_line = buf;
    }

    float gv = s.progress;
    if (s.seek_target_progress > 0.0f) {
        /* After seek fired, keep gauge at target until progress catches up */
        gv = std::max((float)s.progress, s.seek_target_progress);
    } else if (s.seek_indicator != 0 && s.total_time_sec > 0) {
        /* Accumulating: show merged progress + seek delta */
        int merged = s.current_time_sec + s.seek_indicator;
        if (merged < 0) merged = 0;
        if (merged > s.total_time_sec) merged = s.total_time_sec;
        gv = (float)merged / s.total_time_sec;
    }
    return theme_bg(vbox(Elements{
        theme_fg(text(top_line)) | dim,
        gauge(gv) | theme_accent,
    }));
}
