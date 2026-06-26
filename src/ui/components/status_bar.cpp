#include "ui/components/status_bar.h"
#include <cstdio>
using namespace ftxui;

Element render_status_bar(const AppState &s) {
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

    std::string loop_str;
    switch (s.loop_mode) {
    case LoopMode::None:     loop_str = "\u2192"; break;
    case LoopMode::Track:    loop_str = "\u21BA"; break;
    case LoopMode::Playlist: loop_str = "\u21BB"; break;
    }
    std::string vol_str = "Vol:" + std::to_string(s.volume);
    std::string title = s.current_song.title ? s.current_song.title : "";

    return vbox(Elements{
        text(" " + state_str + " " + loop_str + "  " + time_str + "  " + vol_str + "  " + title) | dim,
        gauge(s.progress),
    });
}
