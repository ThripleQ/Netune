#include "state_store.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "plugins/music_sources/netease/netease_api.h"
}

/* Use the C API's song_info_copy/song_info_free (from core/music_source.h,
   included via state_store.h) instead of a local duplicate. */

StateStore& StateStore::instance() {
    static StateStore s;
    return s;
}

/* ── Selection validation ──────────────────────────────
   After any mutation that could leave the selection
   pointing at nothing, clamp to the nearest valid target.
   Cross-panel fallback: if the active panel has no
   selectable items, switch to the other panel. */
void StateStore::validate_selection(void) {
    /* Left panel: groups */
    if (!state_.groups.empty() && state_.group_index >= (int)state_.groups.size())
        state_.group_index = (int)state_.groups.size() - 1;

    /* Right panel: playlist */
    if (!state_.playlist.empty() && state_.selected_index >= (int)state_.playlist.size())
        state_.selected_index = (int)state_.playlist.size() - 1;

    /* If active panel has nothing selectable, try the other */
    if (state_.active_panel == 1 && state_.playlist.empty()) {
        if (!state_.groups.empty() || state_.group_index == -1)
            state_.active_panel = 0;
    }
    if (state_.active_panel == 0 && state_.groups.empty() && state_.group_index >= 0) {
        /* No local groups — fall back to netease entry */
        state_.group_index = -1;
    }
}

void StateStore::set_playback_state(PlaybackState s) {
    state_.playback_state = s;
}

void StateStore::set_current_song(const SongInfo &song) {
    song_info_copy(&state_.current_song, &song);
}

void StateStore::set_progress(double pos, int cur_sec, int total_sec) {
    set_progress_ms(pos, cur_sec * 1000, total_sec);
}

void StateStore::set_progress_ms(double pos, int cur_ms, int total_sec) {
    state_.progress         = pos;
    state_.current_time_sec = cur_ms / 1000;
    state_.current_time_ms  = cur_ms;
    state_.total_time_sec  = total_sec;
}

void StateStore::set_seek_indicator(int delta) {
    state_.seek_indicator = delta;
}

void StateStore::set_seek_target_progress(float p) {
    state_.seek_target_progress = p;
}

void StateStore::set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    state_.volume = vol;
}

void StateStore::set_muted(bool m) {
    state_.muted = m;
}

void StateStore::set_loop_mode(LoopMode mode) {
    state_.loop_mode = mode;
}

/* ── Playback queue ──────────────────────────────────
   The queue is an independent deep copy of the playlist,
   taken when a song starts playing. All auto-advance
   (finish/next/prev) reads it exclusively, so browsing,
   searching, or playlist replacement never disturbs what
   is currently playing. */

void StateStore::queue_snapshot(void) {
    /* free old queue */
    for (auto &s : state_.playback_queue) song_info_free(&s);
    state_.playback_queue.clear();

    for (auto &s : state_.playlist) {
        SongInfo copy = {};
        song_info_copy(&copy, &s);
        state_.playback_queue.push_back(copy);
    }
    state_.queue_index  = state_.selected_index;
    state_.queue_active = !state_.playback_queue.empty();

    /* the shuffle permutation is derived once per playlist change:
       regenerate it whenever the queue is (re)built, regardless of
       the current loop mode, so switching to Shuffle later reuses
       this same ordering */
    shuffle_now();
}

void StateStore::queue_clear(void) {
    for (auto &s : state_.playback_queue) song_info_free(&s);
    state_.playback_queue.clear();
    state_.queue_index  = 0;
    state_.queue_active = false;
    state_.shuffle_order.clear();
    state_.shuffle_pos  = 0;
}

/* ── Shuffle support ─────────────────────────────────
   shuffle_order is a permutation of queue indices, derived
   once per playlist change (see queue_snapshot) and reused
   as-is when loop_mode == LoopMode::Shuffle. shuffle_pos
   tracks the current song's slot in the order. */

void StateStore::shuffle_now(void) {
    int n = (int)state_.playback_queue.size();
    state_.shuffle_order.clear();
    if (n <= 0) return;

    /* start with identity order, then Fisher-Yates */
    state_.shuffle_order.reserve((size_t)n);
    for (int i = 0; i < n; i++) state_.shuffle_order.push_back(i);
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        std::swap(state_.shuffle_order[i], state_.shuffle_order[j]);
    }

    /* pin the current song: find its slot, keep it at shuffle_pos */
    state_.shuffle_pos = 0;
    for (int i = 0; i < n; i++) {
        if (state_.shuffle_order[i] == state_.queue_index) {
            state_.shuffle_pos = i;
            break;
        }
    }
}

/* linear next/prev helper (Track handled by callers) */
static int queue_step_linear(const AppState &st, int dir) {
    int total = (int)st.playback_queue.size();
    if (total <= 0) return -1;
    int idx = st.queue_index;
    if (st.loop_mode == LoopMode::Playlist) {
        if (dir > 0) return (idx + 1 >= total) ? 0 : idx + 1;
        return (idx - 1 < 0) ? total - 1 : idx - 1;
    }
    /* LoopMode::None */
    if (dir > 0) {
        if (idx + 1 >= total) return -1;
        return idx + 1;
    }
    if (idx - 1 < 0) return -1;
    return idx - 1;
}

/* shuffle step: move dir (+1/-1) inside the permutation,
   wrapping at both ends (the permutation cycles as a whole). */
static bool queue_step_shuffle(AppState &st, int dir) {
    if (st.loop_mode != LoopMode::Shuffle || !st.queue_active) return false;
    int n = (int)st.playback_queue.size();
    if (n <= 0 || (int)st.shuffle_order.size() != n) return false;

    int pos = st.shuffle_pos + dir;
    if (pos < 0) pos = n - 1;
    else if (pos >= n) pos = 0;
    st.shuffle_pos  = pos;
    st.queue_index  = st.shuffle_order[pos];
    return true;
}

/* ── Single implementation shared by advance/next/prev ──
   Returns the new queue_index, or -1 when playback should stop.
   Track loop replays the current song; shuffle walks the fixed
   permutation; linear modes walk the queue (Playlist wraps). */
int StateStore::queue_step(int dir) {
    if (!state_.queue_active || state_.playback_queue.empty()) return -1;

    if (state_.loop_mode == LoopMode::Track)
        return state_.queue_index;

    if (state_.loop_mode == LoopMode::Shuffle) {
        if (!queue_step_shuffle(state_, dir)) return -1;
        return state_.queue_index;
    }

    int idx = queue_step_linear(state_, dir);
    if (idx < 0) return -1;
    state_.queue_index = idx;
    return idx;
}

int StateStore::queue_advance(void) {
    return queue_step(+1);
}

int StateStore::queue_next(void) {
    return queue_step(+1);
}

int StateStore::queue_prev(void) {
    return queue_step(-1);
}

const SongInfo* StateStore::queue_current(void) const {
    if (!state_.queue_active) return nullptr;
    if (state_.queue_index < 0 ||
        state_.queue_index >= (int)state_.playback_queue.size())
        return nullptr;
    return &state_.playback_queue[state_.queue_index];
}

void StateStore::set_playlist(const std::vector<SongInfo> &list, int index) {
    /* free old playlist */
    for (auto &s : state_.playlist) {
        song_info_free(&s);
    }
    state_.playlist.clear();

    /* copy new */
    for (auto &s : list) {
        SongInfo copy = {};
        song_info_copy(&copy, &s);
        state_.playlist.push_back(copy);
    }
    state_.selected_index = index;
    validate_selection();
}

void StateStore::set_music_mode(MusicMode mode) {
    state_.music_mode = mode;

    /* Clear playlist when switching modes */
    for (auto &s : state_.playlist) song_info_free(&s);
    state_.playlist.clear();
    state_.selected_index = 0;
    state_.active_panel = 0;
    validate_selection();

    /* When switching to Netease, populate default menu items */
    if (mode == MusicMode::Netease && state_.netease_menu.empty()) {
        state_.netease_menu = {
            {"\u626B\u7801\u767B\u5F55",   200, ""},        /* 扫码登录 */
            {"\u65E5\u5E38\u63A8\u8350",   0, ""},          /* 每日推荐 */
            {"\u63A8\u8350\u6B4C\u5355",   1, ""},          /* 推荐歌单 */
            {"\u6211\u7684\u6B4C\u5355",   2, ""},          /* 我的歌单 */
            {"\u6536\u85CF\u6B4C\u5355",   3, ""},          /* 收藏歌单 */
            {"\u6211\u559C\u6B22\u7684\u97F3\u4E50", 4, ""},  /* 我喜欢的音乐 */
            {"\u641C\u7D22\u7F51\u6613\u4E91", 100, ""},  /* 搜索网易云 */
        };
        /* If already logged in from a previous session, show account name */
        if (netease_is_logged_in()) {
            const char *name = netease_account_name();
            if (name && name[0])
                state_.netease_menu[0].name = name;
            else
                state_.netease_menu[0].name = "\u5df2\u767b\u5f55";
        }
    }
    state_.netease_selected = 0;
}

void StateStore::set_netease_menu(const std::vector<NeteaseMenuItem> &items) {
    state_.netease_menu = items;
}

void StateStore::set_netease_selected(int idx) {
    state_.netease_selected = idx;
}

void StateStore::set_login_state(int st, const std::string &status,
                                   const std::string &qr) {
    state_.login_state = st;
    state_.login_status = status;
    state_.login_qr = qr;
}

void StateStore::set_show_help(bool show) {
    state_.show_help = show;
}

void StateStore::set_loading(bool v) {
    state_.loading = v;
}

void StateStore::set_search_scope(int scope) {
    state_.search_scope = scope;
}

void StateStore::set_search_active(bool active) {
    state_.search_active = active;
}

void StateStore::set_search_query(const std::string &query) {
    state_.search_query = query;
}

void StateStore::set_search_selected(int idx) {
    state_.search_selected = idx;
}

void StateStore::set_search_results(const std::vector<SongInfo> &results, int total) {
    /* free old */
    for (auto &s : state_.search_results) {
        song_info_free(&s);
    }
    state_.search_results.clear();
    state_.search_selected = 0;
    state_.search_total = total;

    for (auto &s : results) {
        SongInfo copy = {};
        song_info_copy(&copy, &s);
        state_.search_results.push_back(copy);
    }
}

void StateStore::set_top_search_active(bool active, int side) {
    state_.top_search_active = active;
    state_.top_search_side   = side;
}

void StateStore::set_top_left_query(const std::string &query) {
    state_.top_left_query = query;
}

void StateStore::set_top_right_query(const std::string &query) {
    state_.top_right_query = query;
}

void StateStore::set_selected_index(int idx) {
    state_.selected_index = idx;
    validate_selection();
}

void StateStore::set_groups(const std::vector<SongGroup> &grps) {
    /* free old groups */
    for (auto &g : state_.groups) {
        for (auto &s : g.songs) song_info_free(&s);
    }
    state_.groups.clear();
    /* Always start from Netease entry (group_index = -1).
       User navigates to local groups with down arrow. */
    state_.group_index = -1;
    state_.selected_index = 0;

    /* copy new */
    for (auto &g : grps) {
        SongGroup copy;
        copy.name = g.name;
        for (auto &s : g.songs) {
            SongInfo si = {};
            song_info_copy(&si, &s);
            copy.songs.push_back(si);
        }
        state_.groups.push_back(std::move(copy));
    }

    /* Only populate right panel if groups exist; otherwise
       keep netease entry selected (group_index stays -1). */
    if (!state_.groups.empty())
        set_group_index(0);
    validate_selection();
}

void StateStore::set_group_index(int idx) {
    state_.group_index = idx;
    if (idx < 0) {
        /* -1 = cross-mode entry (netease), no playlist update */
        validate_selection();
        return;
    }
    if (idx >= (int)state_.groups.size()) { state_.group_index = 0; validate_selection(); return; }
    /* update right panel from this group */
    auto &grp = state_.groups[idx];
    set_playlist(grp.songs, 0);
}

void StateStore::nav_push(void) {
    NavState ns;
    for (auto &s : state_.playlist) {
        SongInfo copy = {};
        song_info_copy(&copy, &s);
        ns.playlist.push_back(copy);
    }
    ns.selected_index   = state_.selected_index;
    ns.active_panel     = state_.active_panel;
    ns.netease_menu     = state_.netease_menu;
    ns.netease_selected = state_.netease_selected;
    ns.search_active    = state_.search_active;
    ns.search_query     = state_.search_query;
    ns.search_scope     = state_.search_scope;
    state_.nav_stack.push_back(std::move(ns));
}

bool StateStore::nav_peek(NavState &out) const {
    if (state_.nav_stack.empty()) return false;
    out = state_.nav_stack.back();  /* shallow: caller must not free songs */
    return true;
}

bool StateStore::nav_pop(void) {
    if (state_.nav_stack.empty()) return false;

    NavState ns = std::move(state_.nav_stack.back());
    state_.nav_stack.pop_back();

    /* Restore navigation state only (left panel menu + focus). The right
       panel list is intentionally NOT restored: its content is replaced
       only by newly loaded content (playlist/menu loads), never by a
       back-navigation. This keeps the visible list stable on Esc. */
    state_.active_panel     = 0;                 /* back to the menu layer */
    state_.netease_menu     = std::move(ns.netease_menu);
    state_.netease_selected = ns.netease_selected;
    /* Esc always exits search mode, never restores it */
    state_.search_active    = false;
    state_.search_query     = "";
    state_.search_scope     = 0;
    validate_selection();
    return true;
}

void StateStore::clear_nav_stack(void) {
    for (auto &ns : state_.nav_stack)
        for (auto &s : ns.playlist)
            song_info_free(&s);
    state_.nav_stack.clear();
}

void StateStore::set_active_panel(int panel) {
    state_.active_panel = (panel == 0 || panel == 1) ? panel : 0;
    validate_selection();
}

void StateStore::set_song_panel_width(int cols) {
    if (cols < 20) cols = 20;
    state_.song_panel_width = cols;
}

void StateStore::set_top_row_width(int cols) {
    if (cols < 30) cols = 30;
    state_.top_row_width = cols;
}

void StateStore::set_screen_height(int rows) {
    if (rows < 10) rows = 10;
    state_.screen_height = rows;
}

void StateStore::set_spectrum(const float *bands) {
    /* dispatched on main thread via event_bus_poll */
    if (bands)
        memcpy(state_.spectrum, bands, sizeof(state_.spectrum));
}

void StateStore::set_lyrics(Lyrics *ly) {
    state_.lyrics = ly;
}

void StateStore::set_lyric_mode(bool mode) {
    state_.lyric_mode = mode;
}

void StateStore::set_cover(const CoverData &cd) {
    /* free old cover pixels */
    free(state_.cover.pixels);
    state_.cover = cd;
}

void StateStore::set_cover_loading(bool v) {
    state_.cover_loading = v;
}
