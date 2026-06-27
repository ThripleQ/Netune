#pragma once

#include <string>
#include <vector>

extern "C" {
#include "core/music_source.h"
}

/* ── Playback state ────────────────────────────────── */
enum class PlaybackState { Stopped, Playing, Paused };

/* ── Loop mode ─────────────────────────────────────── */
enum class LoopMode { None = 0, Track = 1, Playlist = 2 };

/* ── Song group (folder/playlist) ──────────────────── */
struct SongGroup {
    std::string name;
    std::vector<SongInfo> songs;
};

/* ── Full application state ────────────────────────── */
struct AppState {
    /* playback */
    PlaybackState playback_state = PlaybackState::Stopped;
    SongInfo      current_song = {};
    double        progress = 0.0;
    int           current_time_sec = 0;
    int           total_time_sec = 0;

    /* volume */
    int  volume = 80;
    bool muted  = false;

    /* right panel: current view of songs + selection */
    std::vector<SongInfo> playlist;    /* currently shown songs */
    int  selected_index = 0;

    /* left panel: groups (folders) */
    std::vector<SongGroup> groups;
    int  group_index = 0;

    /* panel focus: 0 = left (groups), 1 = right (songs) */
    int  active_panel = 0;

    /* play mode */
    LoopMode loop_mode = LoopMode::None;

    /* help screen */
    bool show_help = false;

    /* search */
    std::string search_keyword;
    std::vector<SongInfo> search_results;
};

/* ── State store singleton ─────────────────────────── */
class StateStore {
public:
    static StateStore& instance();
    const AppState& state() const { return state_; }

    /* playback */
    void set_playback_state(PlaybackState s);
    void set_current_song(const SongInfo &song);
    void set_progress(double pos, int cur_sec, int total_sec);

    /* volume */
    void set_volume(int vol);
    void set_muted(bool m);

    /* groups & playlist */
    void set_groups(const std::vector<SongGroup> &grps);
    void set_group_index(int idx);       /* switch group, updates right panel */
    void set_active_panel(int panel);    /* 0=left, 1=right */
    void set_playlist(const std::vector<SongInfo> &list, int index);
    void set_selected_index(int idx);
    void set_loop_mode(LoopMode mode);

    /* help screen */
    void set_show_help(bool show);

    /* search */
    void set_search_results(const std::string &keyword,
                            const std::vector<SongInfo> &results);

private:
    StateStore() = default;
    AppState state_;
};
