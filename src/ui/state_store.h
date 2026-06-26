#pragma once

#include <string>
#include <vector>
#include <functional>

extern "C" {
#include "core/music_source.h"
}

/* ── Playback state ────────────────────────────────── */
enum class PlaybackState {
    Stopped,
    Playing,
    Paused,
};

/* ── Loop mode ─────────────────────────────────────── */
enum class LoopMode {
    None = 0,
    Track = 1,
    Playlist = 2,
};

/* ── Full application state ────────────────────────── */
struct AppState {
    /* playback */
    PlaybackState playback_state = PlaybackState::Stopped;
    SongInfo      current_song = {};
    double        progress = 0.0;        /* 0.0 ~ 1.0 */
    int           current_time_sec = 0;
    int           total_time_sec = 0;

    /* volume */
    int  volume = 80;                     /* 0 ~ 100 */
    bool muted  = false;

    /* playlist */
    std::vector<SongInfo> playlist;
    int  playlist_index = -1;

    /* play mode */
    LoopMode loop_mode = LoopMode::None;

    /* search */
    std::string           search_keyword;
    std::vector<SongInfo> search_results;

    /* UI */
    std::string active_panel = "browser";
    int         selected_index = 0;
};

/* ── Change callback ───────────────────────────────── */
using StateChangeCallback = std::function<void(const AppState&)>;

/* ── State store singleton ─────────────────────────── */
class StateStore {
public:
    static StateStore& instance();

    const AppState& state() const { return state_; }

    /* mutate (publishes change to subscribers) */
    void set_playback_state(PlaybackState s);
    void set_current_song(const SongInfo &song);
    void set_progress(double pos, int cur_sec, int total_sec);
    void set_volume(int vol);
    void set_muted(bool m);
    void set_loop_mode(LoopMode mode);
    void set_playlist(const std::vector<SongInfo> &list, int index);
    void set_search_results(const std::string &keyword,
                            const std::vector<SongInfo> &results);

    /* subscribe to state changes */
    void subscribe(StateChangeCallback cb);

private:
    StateStore() = default;
    void notify();

    AppState state_;
    std::vector<StateChangeCallback> subscribers_;
};
