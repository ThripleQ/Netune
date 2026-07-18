#include "state_store.h"
#include <algorithm>
#include <cstring>

extern "C" {
#include "plugins/music_sources/netease/netease_api.h"
}

/* helper: copy SongInfo (C struct → C++ member) */
static void copy_song_info(SongInfo &dst, const SongInfo &src) {
    /* free old */
    free(dst.id);
    free(dst.source);
    free(dst.title);
    free(dst.artist);
    free(dst.album);
    free(dst.cover_url);
    free(dst.aux_label);

    auto dup = [](const char *s) -> char* {
        return s ? strdup(s) : nullptr;
    };

    dst = SongInfo{};
    dst.id           = dup(src.id);
    dst.source       = dup(src.source);
    dst.title        = dup(src.title);
    dst.artist       = dup(src.artist);
    dst.album        = dup(src.album);
    dst.duration_sec = src.duration_sec;
    dst.fee          = src.fee;
    dst.cover_url    = dup(src.cover_url);
    dst.aux_label    = dup(src.aux_label);
}

StateStore& StateStore::instance() {
    static StateStore s;
    return s;
}

void StateStore::set_playback_state(PlaybackState s) {
    state_.playback_state = s;
}

void StateStore::set_current_song(const SongInfo &song) {
    copy_song_info(state_.current_song, song);
}

void StateStore::set_progress(double pos, int cur_sec, int total_sec) {
    state_.progress         = pos;
    state_.current_time_sec = cur_sec;
    state_.current_time_ms  = cur_sec * 1000;
    state_.total_time_sec  = total_sec;
}

void StateStore::set_progress_ms(double pos, int cur_ms, int total_sec) {
    state_.progress         = pos;
    state_.current_time_sec = cur_ms / 1000;
    state_.current_time_ms  = cur_ms;
    state_.total_time_sec  = total_sec;
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

void StateStore::set_playlist(const std::vector<SongInfo> &list, int index) {
    /* free old playlist */
    for (auto &s : state_.playlist) {
        song_info_free(&s);
    }
    state_.playlist.clear();

    /* copy new */
    for (auto &s : list) {
        SongInfo copy = {};
        copy_song_info(copy, s);
        state_.playlist.push_back(copy);
    }
    state_.selected_index = index;
}

void StateStore::set_music_mode(MusicMode mode) {
    state_.music_mode = mode;

    /* Clear playlist when switching modes */
    for (auto &s : state_.playlist) song_info_free(&s);
    state_.playlist.clear();
    state_.selected_index = 0;
    state_.active_panel = 0;

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
        copy_song_info(copy, s);
        state_.search_results.push_back(copy);
    }
}

void StateStore::set_selected_index(int idx) {
    state_.selected_index = idx;
}

void StateStore::set_groups(const std::vector<SongGroup> &grps) {
    /* free old groups */
    for (auto &g : state_.groups) {
        for (auto &s : g.songs) song_info_free(&s);
    }
    state_.groups.clear();
    state_.group_index = 0;
    state_.selected_index = 0;

    /* copy new */
    for (auto &g : grps) {
        SongGroup copy;
        copy.name = g.name;
        for (auto &s : g.songs) {
            SongInfo si = {};
            copy_song_info(si, s);
            copy.songs.push_back(si);
        }
        state_.groups.push_back(std::move(copy));
    }

    /* update right panel to first group */
    set_group_index(0);
}

void StateStore::set_group_index(int idx) {
    state_.group_index = idx;
    if (idx < 0) {
        /* -1 = cross-mode entry (netease), no playlist update */
        return;
    }
    if (idx >= (int)state_.groups.size()) { state_.group_index = 0; return; }
    /* update right panel from this group */
    auto &grp = state_.groups[idx];
    set_playlist(grp.songs, 0);
}

void StateStore::nav_push(void) {
    NavState ns;
    for (auto &s : state_.playlist) {
        SongInfo copy = {};
        copy_song_info(copy, s);
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

bool StateStore::nav_pop(void) {
    if (state_.nav_stack.empty()) return false;

    NavState ns = std::move(state_.nav_stack.back());
    state_.nav_stack.pop_back();

    /* free current state */
    for (auto &s : state_.playlist) song_info_free(&s);
    state_.playlist.clear();

    /* restore from saved state */
    state_.playlist         = std::move(ns.playlist);
    state_.selected_index   = ns.selected_index;
    state_.active_panel     = ns.active_panel;
    state_.netease_menu     = std::move(ns.netease_menu);
    state_.netease_selected = ns.netease_selected;
    /* Esc always exits search mode, never restores it */
    state_.search_active    = false;
    state_.search_query     = "";
    state_.search_scope     = 0;
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
}

void StateStore::set_song_panel_width(int cols) {
    if (cols < 20) cols = 20;
    state_.song_panel_width = cols;
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
