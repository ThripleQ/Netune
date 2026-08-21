#pragma once

#include <string>
#include <vector>

extern "C" {
#include "core/music_source.h"
#include "core/lyric.h"
#include "core/cover.h"
#include "core/spectrum.h"
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
    int                        search_scope     = 0;
    bool                       search_active    = false;
    std::string                search_query;
    /* Restore the right-panel list on pop. Set when entering a playlist
       from a playlist list (recommend/my/favorite): Esc then returns to
       the playlist list instead of leaving it behind. */
    bool                       restore_playlist = false;
};

/* ── Loop mode ─────────────────────────────────────── */
enum class LoopMode { None = 0, Track = 1, Playlist = 2, Shuffle = 3 };

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
    int           current_time_ms  = 0;  /* ms precision for lyrics/karaoke */
    int           total_time_sec = 0;
    int           seek_indicator = 0;  /* non-zero = pending seek delta (s) */
    float         seek_target_progress = 0.0f;  /* after seek fires, gauge stays here until progress catches up */

    /* volume */
    int  volume = 80;
    bool muted  = false;

    /* right panel: current view of songs + selection */
    std::vector<SongInfo> playlist;    /* currently shown songs */
    int  selected_index = 0;

    /* playback queue: snapshot taken when a song starts playing.
       Auto-advance (finish/next/prev) reads ONLY this, never `playlist`,
       so browsing/searching elsewhere doesn't disturb playback. */
    std::vector<SongInfo> playback_queue;
    int  queue_index = 0;             /* position in playback_queue */
    bool queue_active = false;        /* true while a queue is playing */

    /* shuffle: fixed permutation over playback_queue.
       Used only when loop_mode == LoopMode::Shuffle.
       shuffle_pos is the current song's slot in the order. */
    std::vector<int> shuffle_order;
    int              shuffle_pos = 0;

    /* left panel: groups (folders) */
    std::vector<SongGroup> groups;
    int  group_index = -1;  /* -1 = netease entry selected */

    /* panel focus: 0 = left (groups), 1 = right (songs) */
    int  active_panel = 0;

    /* play mode */
    LoopMode loop_mode = LoopMode::None;

    /* source mode — default to Netease so the app opens on the music
       service homepage (local files remain reachable via the menu) */
    MusicMode music_mode = MusicMode::Netease;

    /* netease menu (shown in left panel when music_mode == Netease) */
    std::vector<NeteaseMenuItem> netease_menu;
    int netease_selected = 0;

    /* netease login */
    int  login_state = 0; /* 0=idle, 1=get_key, 2=wait_scan, 3=done, -1=error */
    std::string login_status; /* status message displayed in overlay */
    std::string login_qr;     /* QR code text for terminal display */
    long login_qr_deadline = 0; /* unix ts when the QR expires (0 = unknown) */
    int  login_net_error = 0;   /* non-zero while polling keeps failing */
    int  qr_gfx_ready = 0;      /* kitty QR image decoded & ready to place */

    /* help screen */
    bool show_help = false;

    /* action sheet (Ctrl+X): like song / subscribe playlist */
    bool action_sheet_open = false;
    int  action_sheet_selected = 0;
    /* -1 = status querying, 0 = not liked/subscribed, 1 = liked/subscribed */
    int  action_sheet_active = -1;
    /* state machine: 0=main menu, 1=playlist picker, 2=text input, 3=confirm */
    int  action_sheet_menu = 0;
    int  action_sheet_opt_count = 3;  /* number of options in menu 0 */
    std::string action_sheet_input;   /* text input buffer */
    std::string action_sheet_ctx;     /* operation context (song id / playlist id) */
    std::vector<SongInfo> action_sheet_pls;  /* my playlists for the picker */

    /* current playlist context (for "remove from this playlist") */
    std::string current_playlist_id;
    bool        detail_playlist_mine = false;  /* open playlist is user-owned */

    /* spectrum */
    float spectrum[SPECTRUM_BANDS] = {0};

    /* lyrics & cover */
    Lyrics    *lyrics      = nullptr;
    bool       lyric_mode  = false;     /* full-screen lyrics view */
    CoverData  cover       = {};        /* current song cover art */
    bool       cover_loading = false;   /* async download in progress */

    /* loading state (for async operations like playlist load) */
    bool loading = false;

    /* marquee width: computed from terminal size, updated per-frame */
    int  song_panel_width = 50;

    /* terminal height, updated per-frame (drives spectrum bar rows) */
    int  screen_height = 24;

    /* top search row width: full terminal width, updated per-frame */
    int  top_row_width = 80;

    /* search: scope=0 filter in-list, scope=1 global search
       (local mode only — netease mode uses the top search row below) */
    int  search_scope = 0;
    bool search_active = false;
    std::string search_query;
    std::vector<SongInfo> search_results;
    int search_selected = 0;
    int search_total = 0;

    /* top search row (netease mode only, rendered in top_bar slot) */
    bool        top_search_active = false;  /* editing a top search box */
    int         top_search_side   = 0;      /* 0 = left box, 1 = right box */
    bool        top_search_api    = false;  /* box is in "Netease API search" mode (from the 搜索网易云 menu entry) vs. plain filter */
    std::string top_left_query;             /* left box: filters netease menu items */
    std::string top_right_query;            /* right box: filters playlist / netease API */

    /* navigation stack for Esc-back */
    std::vector<NavState> nav_stack;
};


/* ── State store singleton ─────────────────────────── */
class StateStore {
public:
    static StateStore& instance();
    const AppState& state() const { return state_; }

    /* selection validation */
    void validate_selection(void);

    /* playback */
    void set_playback_state(PlaybackState s);
    void set_current_song(const SongInfo &song);
    void set_progress(double pos, int cur_sec, int total_sec);
    void set_progress_ms(double pos, int cur_ms, int total_sec);
    void set_seek_indicator(int delta);
    void set_seek_target_progress(float p);

    /* volume */
    void set_volume(int vol);
    void set_muted(bool m);

    /* groups & playlist */
    void set_groups(const std::vector<SongGroup> &grps);
    void set_group_index(int idx);       /* switch group, updates right panel */
    void set_active_panel(int panel);    /* 0=left, 1=right */
    void set_song_panel_width(int cols);
    void set_top_row_width(int cols);
    void set_screen_height(int rows);
    void set_playlist(const std::vector<SongInfo> &list, int index);
    void set_selected_index(int idx);
    void set_loop_mode(LoopMode mode);
    void set_music_mode(MusicMode mode);
    void set_netease_menu(const std::vector<NeteaseMenuItem> &items);
    void set_netease_selected(int idx);
    void set_login_state(int state, const std::string &status, const std::string &qr);
    void set_login_deadline(long unix_ts);
    void set_login_net_error(int on);
    void set_qr_gfx_ready(int ready);

    /* playback queue */
    /* snapshot current playlist into the playback queue (call when a song
       starts playing); deep-copies so later playlist changes don't affect it */
    void queue_snapshot(void);
    void queue_clear(void);              /* stop queue, free snapshot */
    /* next/prev inside the queue honoring loop_mode and shuffle.
       Returns the new queue_index, or -1 when playback should stop. */
    int  queue_advance(void);            /* auto-advance after finish */
    int  queue_next(void);               /* manual Next key */
    int  queue_prev(void);               /* manual Prev key */
    /* current queue song, or nullptr if queue inactive/empty */
    const SongInfo* queue_current(void) const;
    /* shuffle permutation helpers (used when loop_mode == Shuffle) */
    void shuffle_now(void);              /* re-shuffle, keep current song fixed */

    /* spectrum */
    void set_spectrum(const float *bands);

    /* lyrics & cover */
    void set_lyrics(Lyrics *ly);
    void set_lyric_mode(bool mode);
    void set_cover(const CoverData &cd);
    void set_cover_loading(bool v);

    /* help screen */
    void set_show_help(bool show);
    void set_action_sheet(bool open, int selected);
    void set_action_sheet_active(int active);
    void set_action_sheet_menu(int menu);
    void set_action_sheet_opt_count(int n);
    void set_action_sheet_input(const std::string &text);
    void set_action_sheet_ctx(const std::string &ctx);
    void set_action_sheet_pls(const std::vector<SongInfo> &pls);
    void set_current_playlist_id(const std::string &id);
    void set_detail_playlist_mine(bool v);

    /* loading */
    void set_loading(bool v);
    bool get_loading(void) const { return state_.loading; }

    /* search */
    void set_search_scope(int scope);
    void set_search_active(bool active);
    void set_search_query(const std::string &query);
    void set_search_selected(int idx);
    void set_search_results(const std::vector<SongInfo> &results, int total);

    /* top search row (netease mode) */
    void set_top_search_active(bool active, int side);
    void set_top_search_api(bool api);
    void set_top_left_query(const std::string &query);
    void set_top_right_query(const std::string &query);

    /* nav stack push/pop for Esc-back */
    void nav_push(void);
    /* push with restore_playlist=true: the right-panel list snapshot is
       restored on pop (used when entering a playlist's songs from a
       playlist list, so Esc lands back on the playlist list) */
    void nav_push_restore_playlist(void);
    bool nav_pop(void);  /* returns true if state restored */
    bool nav_peek(NavState &out) const;  /* shallow copy of top; false if empty */
    void clear_nav_stack(void);

private:
    StateStore() = default;
    AppState state_;

    /* shared core of queue_advance/next/prev (+1 next / -1 prev) */
    int queue_step(int dir);
};
