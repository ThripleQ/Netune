#include "ui/components/status_bar.h"
#include "ui/components/theme_util.h"
#include <cstdio>
#include <string>
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

    return theme_bg(vbox(Elements{
        theme_fg(text(top_line)) | dim,
        gauge(s.progress) | theme_accent,
    }));
}
