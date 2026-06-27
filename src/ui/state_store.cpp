#include "state_store.h"
#include <algorithm>
#include <cstring>

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
    dst.cover_url    = dup(src.cover_url);
    dst.aux_label    = dup(src.aux_label);
}

StateStore& StateStore::instance() {
    static StateStore s;
    return s;
}

void StateStore::notify() {
    for (auto &cb : subscribers_) {
        cb(state_);
    }
}

void StateStore::set_playback_state(PlaybackState s) {
    state_.playback_state = s;
    notify();
}

void StateStore::set_current_song(const SongInfo &song) {
    copy_song_info(state_.current_song, song);
    notify();
}

void StateStore::set_progress(double pos, int cur_sec, int total_sec) {
    state_.progress        = pos;
    state_.current_time_sec = cur_sec;
    state_.total_time_sec   = total_sec;
    notify();
}

void StateStore::set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    state_.volume = vol;
    notify();
}

void StateStore::set_muted(bool m) {
    state_.muted = m;
    notify();
}

void StateStore::set_loop_mode(LoopMode mode) {
    state_.loop_mode = mode;
    notify();
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
    notify();
}

void StateStore::set_show_help(bool show) {
    state_.show_help = show;
    notify();
}

void StateStore::set_search_results(const std::string &keyword,
                                     const std::vector<SongInfo> &results) {
    /* free old */
    for (auto &s : state_.search_results) {
        song_info_free(&s);
    }
    state_.search_results.clear();
    state_.search_keyword = keyword;

    for (auto &s : results) {
        SongInfo copy = {};
        copy_song_info(copy, s);
        state_.search_results.push_back(copy);
    }
    notify();
}

void StateStore::set_selected_index(int idx) {
    state_.selected_index = idx;
    notify();
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
    if (idx < 0 || idx >= (int)state_.groups.size()) return;
    state_.group_index = idx;
    /* update right panel from this group */
    auto &grp = state_.groups[idx];
    set_playlist(grp.songs, 0);
}

void StateStore::set_active_panel(int panel) {
    state_.active_panel = (panel == 0 || panel == 1) ? panel : 0;
    notify();
}

void StateStore::subscribe(StateChangeCallback cb) {
    subscribers_.push_back(std::move(cb));
}
