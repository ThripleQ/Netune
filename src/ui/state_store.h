#pragma once

#include <string>
#include <vector>

extern "C" {
#include "core/music_source.h"
}

/* ── Playback state ────────────────────────────────── */
enum class PlaybackState { Stopped, Playing, Paused };

/* ── Music source mode ─────────────────────────────── */
enum class MusicMode { Local, Netease };

/* ── Netease menu item ─────────────────────────────── */
struct NeteaseMenuItem {
    std::string name;
    int         type;  /* 0=daily, 1=recommended playlists, 2=my playlists, 3=favorites, 100=search */
    std::string id;    /* playlist id for types 1-3 */
};

/* ── Navigation state (for Esc-back) ────────────── */
struct NavState {
    std::vector<SongInfo>      playlist;
    int                        selected_index  = 0;
    int                        active_panel    = 0;
    std::vector<NeteaseMenuItem> netease_menu;
    int                        netease_selected = 0;
    bool                       search_active    = false;
    std::string                search_query;
};

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

    /* source mode */
    MusicMode music_mode = MusicMode::Local;

    /* netease menu (shown in left panel when music_mode == Netease) */
    std::vector<NeteaseMenuItem> netease_menu;
    int netease_selected = 0;

    /* netease login */
    int  login_state = 0; /* 0=idle, 1=get_key, 2=wait_scan, 3=done, -1=error */
    std::string login_status; /* status message displayed in overlay */
    std::string login_qr;     /* QR code text for terminal display */

    /* help screen */
    bool show_help = false;

    /* loading state (for async operations like playlist load) */
    bool loading = false;

    /* marquee width: computed from terminal size, updated per-frame */
    int  song_panel_width = 50;

    /* search */
    bool search_active = false;
    std::string search_query;
    std::vector<SongInfo> search_results;
    int search_selected = 0;
    int search_total = 0;
    /* navigation stack for Esc-back */
    std::vector<NavState> nav_stack;
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
    void set_song_panel_width(int cols);
    void set_playlist(const std::vector<SongInfo> &list, int index);
    void set_selected_index(int idx);
    void set_loop_mode(LoopMode mode);
    void set_music_mode(MusicMode mode);
    void set_netease_menu(const std::vector<NeteaseMenuItem> &items);
    void set_netease_selected(int idx);
    void set_login_state(int state, const std::string &status, const std::string &qr);

    /* help screen */
    void set_show_help(bool show);

    /* loading */
    void set_loading(bool v);
    bool get_loading(void) const { return state_.loading; }

    /* search */
    void set_search_active(bool active);
    void set_search_query(const std::string &query);
    void set_search_selected(int idx);
    void set_search_results(const std::vector<SongInfo> &results, int total);
    /* nav stack push/pop for Esc-back */
    void nav_push(void);
    bool nav_pop(void);  /* returns true if state restored */

private:
    StateStore() = default;
    AppState state_;
};
