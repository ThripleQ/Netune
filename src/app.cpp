#include "app.h"
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

extern "C" {
#include "infra/log.h"
#include "infra/config.h"
#include "core/event_bus.h"
#include "core/playback_coordinator.h"
#include "core/music_source_manager.h"
#include "core/music_source.h"
#include "core/playlist_manager.h"
#include "core/audio_output_mgr.h"
#include "core/search_manager.h"
#include "core/cache_manager.h"
#include "plugins/music_sources/local/local_source.h"
#include "plugins/music_sources/netease/netease_source.h"
#include "plugins/music_sources/netease/netease_api.h"
}

#include "ui/state_store.h"
#include "ui/keybindings.h"
#include "ui/components/top_bar.h"
#include "ui/components/status_bar.h"
#include "ui/components/group_list.h"
#include "ui/components/song_list.h"
#include "ui/components/player_controls.h"
#include "ui/components/help_screen.h"
#include "ui/components/search_bar.h"
#include "ui/components/login_screen.h"
#include "ui/theme.h"
#include "ui/layout_engine.h"

using namespace ftxui;

/* ── Global keybinding manager ──────────────────────── */
static KeybindingManager g_keybindings;

static volatile bool g_running = true;

static void on_signal(int sig) { (void)sig; g_running = false; }

/* ── Login state ────────────────────────────────────── */
static std::string g_login_unikey;
static int         g_login_poll_tick = 0;

/* Generate QR code text using netease-cli's built-in qr-render */
static std::string gen_qr(const char *url) {
    char *qr = netease_qr_render(url);
    if (!qr) return "";
    std::string s(qr);
    free(qr);
    return s;
}

/* Start the QR login flow using netease-cli */
static void start_login(void) {
    StateStore::instance().set_login_state(1, "Contacting server...", "");
    char unikey[128] = {0};
    char qr_url[512] = {0};
    if (netease_qr_get_key(unikey, sizeof(unikey), qr_url, sizeof(qr_url)) == 0
        && unikey[0]) {
        g_login_unikey = unikey;
        g_login_poll_tick = 0;
        std::string qr = gen_qr(qr_url);
        StateStore::instance().set_login_state(2,
            "Scan with Netease Music App", qr);
    } else {
        StateStore::instance().set_login_state(-1,
            "Failed to get QR code", "");
    }
}

/* Update netease menu after successful login */
static void update_login_menu(void) {
    const auto &cur = StateStore::instance().state();
    if (cur.netease_menu.empty()) return;
    const char *name = netease_account_name();
    if (!name) name = "Logged in";
    auto menu = cur.netease_menu;
    std::string label = "";
    label += name;
    menu[0].name = label;
    StateStore::instance().set_netease_menu(menu);
}

#include <map>

/* ── Netease search cache (in-memory LRU, max 32 entries) ── */
static std::map<std::string, std::vector<SongInfo>> g_ns_cache;
#define NS_CACHE_MAX 32

static void do_netease_search(const char *query) {
    if (!query || !query[0]) return;
    std::string q(query);

    /* Check cache first */
    auto it = g_ns_cache.find(q);
    if (it != g_ns_cache.end()) {
        StateStore::instance().set_search_results(it->second, (int)it->second.size());
        return;
    }

    NeteaseSearchResult nr;
    if (netease_search(query, 100, 0, &nr) != 0) return;

    std::vector<SongInfo> vec;
    vec.reserve(nr.count);
    for (int i = 0; i < nr.count; i++) {
        SongInfo si = {};
        si.id       = strdup(nr.songs[i].id);
        si.source   = strdup("netease");
        si.title    = strdup(nr.songs[i].title ? nr.songs[i].title : "");
        si.artist   = strdup(nr.songs[i].artist ? nr.songs[i].artist : "");
        si.album    = strdup(nr.songs[i].album ? nr.songs[i].album : "");
        si.duration_sec = nr.songs[i].duration_ms / 1000;
        vec.push_back(si);
    }
    netease_search_result_free(&nr);

    /* Store in cache, evict oldest if full */
    if (g_ns_cache.size() >= NS_CACHE_MAX)
        g_ns_cache.erase(g_ns_cache.begin());
    g_ns_cache[q] = vec;

    /* Deep copy for StateStore (vec holds shallow ownership) */
    StateStore::instance().set_search_results(vec, (int)vec.size());
    fprintf(stderr, "\nDIAG: search loaded %zu results\n", vec.size());
}

/* Free search cache on shutdown */
static void free_ns_cache(void) {
    for (auto &entry : g_ns_cache)
        for (auto &s : entry.second)
            song_info_free(&s);
    g_ns_cache.clear();
}

/* ── Search event → StateStore bridge ─────────────── */
static void ev_search_start(const BusEvent *ev, void *data) {
    (void)data;
    StateStore::instance().set_search_active(true);
    StateStore::instance().set_search_query(
        ev->data ? (const char*)ev->data : "");
}

static void ev_search_result(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    const SearchResult *sr = search_manager_results();
    if (sr && sr->count > 0) {
        /* Build vector with proper deep copies */
        std::vector<SongInfo> vec;
        vec.reserve(sr->count);
        for (int i = 0; i < sr->count; i++) {
            SongInfo copy = {};
            song_info_copy(&copy, &sr->songs[i]);
            vec.push_back(copy);
        }
        StateStore::instance().set_search_results(vec, sr->total);
        /* clean up the copies after set_search_results re-copies them */
        for (auto &v : vec) song_info_free(&v);
    }
}

static void ev_search_error(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    StateStore::instance().set_search_active(false);
    StateStore::instance().set_search_query("");
}

/* ── Event bus → StateStore bridge ────────────────── */
static void ev_progress(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int[3])) {
        int *p = (int*)ev->data;
        double prog = (p[1] > 0) ? (double)p[0] / p[1] : 0.0;
        int cur = (p[2] > 0) ? p[0] / p[2] : 0;
        int tot = (p[2] > 0 && p[1] > 0) ? p[1] / p[2] : 0;
        StateStore::instance().set_progress(prog, cur, tot);
    }
}
static void ev_playback_start(const BusEvent *ev, void *data) {
    (void)ev; (void)data; StateStore::instance().set_playback_state(PlaybackState::Playing);
}
static void ev_playback_pause(const BusEvent *ev, void *data) {
    (void)ev; (void)data; StateStore::instance().set_playback_state(PlaybackState::Paused);
}
static void ev_playback_resume(const BusEvent *ev, void *data) {
    (void)ev; (void)data; StateStore::instance().set_playback_state(PlaybackState::Playing);
}
static void ev_playback_stop(const BusEvent *ev, void *data) {
    (void)ev; (void)data; StateStore::instance().set_playback_state(PlaybackState::Stopped); StateStore::instance().set_progress(0,0,0);
}
static void ev_playback_error(const BusEvent *ev, void *data) {
    (void)ev; (void)data; LOG_WARN("Playback error"); StateStore::instance().set_playback_state(PlaybackState::Stopped); StateStore::instance().set_progress(0,0,0);
}
static void ev_playback_finish(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    int next = playlist_manager_advance();
    if (next >= 0) {
        const char *path = playlist_manager_get_path(next);
        if (path) {
            const auto &st = StateStore::instance().state();
            StateStore::instance().set_selected_index(next);
            if (next < (int)st.playlist.size()) {
                StateStore::instance().set_current_song(st.playlist[next]);
                event_bus_publish(EV_TRACK_CHANGED, &next, sizeof(next));
            }
            event_bus_publish(EV_PLAYBACK_START, (void*)path, strlen(path) + 1);
            return;
        }
    }
    StateStore::instance().set_playback_state(PlaybackState::Stopped);
}

/* ── Volume / Mute / Playlist event handlers ──────── */
static void ev_volume_changed(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int)) {
        int vol = *(int*)ev->data;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        audio_output_set_volume(vol);
        StateStore::instance().set_volume(vol);
    }
}

static void ev_mute_changed(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int)) {
        int muted = *(int*)ev->data;
        StateStore::instance().set_muted(muted != 0);
        int target = muted ? 0 : StateStore::instance().state().volume;
        if (target <= 0) target = 80;
        audio_output_set_volume(target);
    }
}

static void ev_track_changed(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    /* Track changed — placeholder for future listeners (lyrics, cover).
       StateStore already has the correct song from direct writes. */
}

static void ev_playlist_changed(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int)) {
        int mode = *(int*)ev->data;
        playlist_manager_set_loop_mode(mode);
        StateStore::instance().set_loop_mode((LoopMode)mode);
    }
}
/* ───────────────────────────────────────────────────── */

int run_app(int argc, char **argv) {
    log_init(NULL);
    LOG_INFO("Netune v2.0.0 starting");

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Netune v2.0.0 — Terminal music player\nUsage: %s [config.json]\n", argv[0]);
        return 0;
    }

    Config *cfg = NULL;
    const char *cfg_path = "data/config.json";
    if (argc > 1) cfg_path = argv[1];
    cfg = config_load(cfg_path);
    if (!cfg) LOG_WARN("No config loaded, using defaults");
    config_set_global(cfg);

    /* init search infrastructure */
    {
        const char *home = getenv("HOME");
        char cache_dir[512];
        if (home) {
            snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/lmusic", home);
        } else {
            snprintf(cache_dir, sizeof(cache_dir), "/tmp/netune-cache");
        }
        cache_init(cache_dir);
    }
    search_manager_init();

    event_bus_init();

    event_bus_subscribe(EV_PROGRESS_UPDATE,   ev_progress, NULL);
    event_bus_subscribe(EV_PLAYBACK_START,    ev_playback_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE,    ev_playback_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME,   ev_playback_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP,     ev_playback_stop, NULL);
    event_bus_subscribe(EV_PLAYBACK_FINISH,   ev_playback_finish, NULL);
    event_bus_subscribe(EV_PLAYBACK_ERROR,    ev_playback_error, NULL);
    event_bus_subscribe(EV_VOLUME_CHANGED,    ev_volume_changed, NULL);
    event_bus_subscribe(EV_MUTE_CHANGED,      ev_mute_changed, NULL);
    event_bus_subscribe(EV_PLAYLIST_CHANGED,  ev_playlist_changed, NULL);
    event_bus_subscribe(EV_TRACK_CHANGED,  ev_track_changed, NULL);
    /* search events — StateStore bridge */
    event_bus_subscribe(EV_SEARCH_START, ev_search_start, NULL);
    event_bus_subscribe(EV_SEARCH_RESULT, ev_search_result, NULL);
    event_bus_subscribe(EV_SEARCH_ERROR, ev_search_error, NULL);

    if (cfg) {
        int vol = config_get_int(cfg, "audio.volume", -1);
        if (vol >= 0 && vol <= 100) StateStore::instance().set_volume(vol);
        int loop = config_get_int(cfg, "playback.loop_mode", 0);
        if (loop >= 0 && loop <= 2) {
            playlist_manager_set_loop_mode(loop);
            StateStore::instance().set_loop_mode((LoopMode)loop);
        }
    }

    /* load keybindings */
    const char *kb_name = config_get_str(cfg, "ui.keybindings", NULL);
    const char *kb_path = "data/keybindings/default.yaml";
    if (kb_name && strcmp(kb_name, "default") != 0) kb_path = kb_name;
    g_keybindings.load(kb_path);

    /* load theme */
    const char *t_name = config_get_str(cfg, "ui.theme", NULL);
    const char *t_path = "data/themes/default.yaml";
    if (t_name && strcmp(t_name, "default") != 0) t_path = t_name;
    ThemeManager::instance().load(t_path);

    /* layout engine */
    LayoutEngine layout_engine;
    layout_engine.register_component("top_bar", render_top_bar);
    layout_engine.register_component("status_bar", render_status_bar);
    layout_engine.register_component("group_list", render_group_list);
    layout_engine.register_component("song_list", render_song_list);
    layout_engine.register_component("player_controls", render_player_controls);
    const char *l_name = config_get_str(cfg, "ui.layout", NULL);
    const char *l_path = "data/layouts/default.yaml";
    if (l_name && strcmp(l_name, "default") != 0) l_path = l_name;
    layout_engine.load(l_path);

    music_source_manager_init();
    local_source_register();
    netease_source_register();

    /* auto-scan */
    {
        std::vector<std::string> scan_dirs;
        int ndirs = cfg ? config_get_array_size(cfg, "music_sources.local.dirs") : 0;
        if (ndirs > 0) {
            for (int i = 0; i < ndirs; i++) {
                char key[64];
                snprintf(key, sizeof(key), "music_sources.local.dirs[%d]", i);
                const char *d = config_get_str(cfg, key, NULL);
                if (d) scan_dirs.push_back(d);
            }
        }
        if (scan_dirs.empty()) {
            const char *home = getenv("HOME");
            if (home) scan_dirs.push_back(home);
        }
        std::vector<SongGroup> groups;
        for (auto &dir : scan_dirs) {
            SearchResult result;
            memset(&result, 0, sizeof(result));
            if (music_source_search("local", dir.c_str(), 0, 0, &result) != 0 || result.count <= 0) {
                if (result.songs) free(result.songs);
                continue;
            }
            SongGroup g;
            const char *slash = strrchr(dir.c_str(), '/');
            g.name = slash ? slash + 1 : dir.c_str();
            for (int j = 0; j < result.count; j++) {
                SongInfo copy;
                song_info_copy(&copy, &result.songs[j]);
                g.songs.push_back(copy);
                song_info_free(&result.songs[j]);
            }
            free(result.songs);
            groups.push_back(std::move(g));
        }
        if (!groups.empty()) {
            StateStore::instance().set_groups(groups);
            /* sync first group's paths to backend */
            {
                const auto &st = StateStore::instance().state();
                std::vector<const char*> paths;
                for (auto &s : st.groups[0].songs) paths.push_back(s.id);
                playlist_manager_sync(paths.data(), (int)paths.size());
                playlist_manager_set_index(0);
            }
            LOG_INFO("Scanned %zu groups", groups.size());
        } else {
            LOG_WARN("No music files found");
        }
    }

    playback_coordinator_init();

    event_bus_publish(EV_APP_STARTUP, NULL, 0);
    event_bus_poll();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    auto screen = ScreenInteractive::Fullscreen();

    std::atomic<bool> timer_active{true};
    std::thread refresh_timer([&]() {
        while (timer_active.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            screen.RequestAnimationFrame();
        }
    });

    auto &state = StateStore::instance();

    auto component = Renderer([&]() -> Element {
        event_bus_poll();
        const AppState &s = state.state();

        state.set_song_panel_width(screen.dimx() - 29);

        /* Login polling: every ~2s while waiting for QR scan;
           auto-close 2s after successful login */
        if (s.login_state == 3) {
            static int close_tick = 0;
            if (++close_tick >= 125) {
                close_tick = 0;
                StateStore::instance().set_login_state(0, "", "");
            }
        }
        if (s.login_state == 2 && ++g_login_poll_tick % 125 == 0) {
            int rc = netease_qr_poll(g_login_unikey.c_str());
            LOG_INFO("LOGIN POLL: rc=%d", rc);
            if (rc == 0) {
                /* 803: authorized, login successful */
                StateStore::instance().set_login_state(3,
                    netease_account_name() ? netease_account_name() : "Logged in", "");
                update_login_menu();
            } else if (rc == 2) {
                /* 800: expired — restart */
                g_login_unikey.clear();
                start_login();
            } else if (rc == 3) {
                /* 802: scanned, waiting for phone confirm */
                StateStore::instance().set_login_state(2,
                    "Scanned. Confirm in Netease Music App...", s.login_qr);
            }
        }

        Element main = vbox(Elements{
            layout_engine.build(s) | flex,
        });

        if (s.login_state != 0) {
            /* Full-page login screen */
            return render_login_screen(s);
        }
        if (s.search_active) {
            main = vbox(Elements{
                main,
                render_search_bar(s) | center | clear_under,
            });
        } else if (s.show_help) {
            /* overlay help screen centered on top of main content */
            main = vbox(Elements{
                main,
                render_help_screen(s) | center | clear_under,
            });
        }

        return main;
    });

    component |= CatchEvent([&](ftxui::Event event) -> bool {
        std::string ev_key;
        if (event == ftxui::Event::Backspace) { ev_key = "backspace"; }
        else if (event == ftxui::Event::Tab) { ev_key = "tab"; }
        else if (event == ftxui::Event::Return) { ev_key = "enter"; }
        else if (event == ftxui::Event::Escape) { ev_key = "escape"; }
        else if (event == ftxui::Event::ArrowUp) { ev_key = "up"; }
        else if (event == ftxui::Event::ArrowDown) { ev_key = "down"; }
        else if (event == ftxui::Event::ArrowLeft) { ev_key = "left"; }
        else if (event == ftxui::Event::ArrowRight) { ev_key = "right"; }
        else if (event.is_character()) {
            ev_key = event.character();
            /* Space: event.character() returns " ", normalize to "space" */
            if (ev_key == " ") ev_key = "space";
        }
        if (ev_key.empty()) return false;

        const AppState &cur = state.state();

        /* ── Login overlay: Esc to close ── */
        if (cur.login_state != 0 && (ev_key == "escape")) {
            StateStore::instance().set_login_state(0, "", "");
            g_login_unikey.clear();
            return true;
        }

        /* ── Search input mode: capture keys as query text ── */
        if (cur.search_active) {
            /* Navigate results */
            if (ev_key == "up" && !cur.search_results.empty()) {
                int idx = cur.search_selected - 1;
                if (idx < 0) idx = (int)cur.search_results.size() - 1;
                StateStore::instance().set_search_selected(idx);
                return true;
            }
            if (ev_key == "down" && !cur.search_results.empty()) {
                int idx = cur.search_selected + 1;
                if (idx >= (int)cur.search_results.size()) idx = 0;
                StateStore::instance().set_search_selected(idx);
                return true;
            }
            /* Enter: submit search in netease mode, navigate in local mode */
            if (ev_key == "enter" || ev_key == "\r") {
                if (cur.music_mode == MusicMode::Netease) {
                    if (!cur.search_query.empty()) {
                        /* submit search to API */
                        do_netease_search(cur.search_query.c_str());
                    }
                    return true;
                }
                /* Local mode: navigate to selected result's folder */
                if (!cur.search_results.empty()) {
                    const auto &sel = cur.search_results[cur.search_selected];
                    if (sel.source && strcmp(sel.source, "local") == 0 &&
                        sel.id && sel.id[0]) {
                        int target_group = -1;
                        int target_song = -1;
                        for (int gi = 0; gi < (int)cur.groups.size() && target_group < 0; gi++) {
                            for (int si = 0; si < (int)cur.groups[gi].songs.size(); si++) {
                                if (strcmp(cur.groups[gi].songs[si].id, sel.id) == 0) {
                                    target_group = gi;
                                    target_song = si;
                                    break;
                                }
                            }
                        }
                        if (target_group >= 0) {
                            std::vector<const char*> paths;
                            for (auto &s : cur.groups[target_group].songs)
                                paths.push_back(s.id);
                            playlist_manager_sync(paths.data(), (int)paths.size());
                            playlist_manager_set_index(target_song);
                            StateStore::instance().set_group_index(target_group);
                            StateStore::instance().set_selected_index(target_song >= 0 ? target_song : 0);
                            StateStore::instance().set_active_panel(1);
                        }
                    }
                    search_manager_clear();
                    StateStore::instance().set_search_active(false);
                    StateStore::instance().set_search_query("");
                }
                return true;
            }
            if (ev_key == "escape") {
                /* close search */
                search_manager_clear();
                StateStore::instance().set_search_active(false);
                StateStore::instance().set_search_query("");
                return true;
            }
            if (ev_key == "backspace") {
                /* remove last char (UTF-8 safe) */
                std::string q = cur.search_query;
                if (!q.empty()) {
                    int n = (int)q.size() - 1;
                    while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80) n--;
                    q.resize((size_t)n);
                }
                StateStore::instance().set_search_query(q);
                StateStore::instance().set_search_results({}, 0);
                /* Local: real-time; Netease: wait for Enter */
                if (!q.empty() && cur.music_mode != MusicMode::Netease)
                    search_manager_search_source("local", q.c_str(), 0);
                return true;
            }
            /* Regular ASCII or UTF-8 character: append to query */
            bool is_ascii = (ev_key.size() == 1 && ev_key[0] >= 32 && ev_key[0] < 127);
            bool is_utf8  = (ev_key.size() > 1 && ((unsigned char)ev_key[0] & 0x80));
            if (is_ascii || is_utf8) {
                std::string ch = ev_key;
                if (ev_key == "space") ch = " ";
                std::string q = cur.search_query + ch;
                StateStore::instance().set_search_query(q);
                StateStore::instance().set_search_results({}, 0);
                /* Local: real-time; Netease: wait for Enter */
                if (q.size() == 1 && cur.music_mode != MusicMode::Netease)
                    search_manager_search_source("local", q.c_str(), 0);
                if (cur.music_mode != MusicMode::Netease && q.size() > 1)
                    search_manager_search_source("local", q.c_str(), 0);
                return true;
            }
            return true; /* consume all keys while searching */
        }

        auto action = g_keybindings.lookup(ev_key);
        if (!action.has_value()) return false;

        switch (action.value()) {

        case Action::Quit:
            screen.ExitLoopClosure()();
            return true;

        case Action::PanelSwitch:
            StateStore::instance().set_active_panel(cur.active_panel ? 0 : 1);
            return true;

        case Action::MoveDown:
            if (cur.active_panel == 0) {
                if (cur.music_mode == MusicMode::Local) {
                    int idx = cur.group_index;
                    if (idx >= (int)cur.groups.size() - 1) return true; /* at bottom */
                    if (idx == -1 && cur.groups.empty()) return true;
                    int next = (idx < 0) ? 0 : idx + 1;
                    StateStore::instance().set_group_index(next);
                    if (next >= 0 && next < (int)cur.groups.size()) {
                        std::vector<const char*> paths;
                        for (auto &s : cur.groups[next].songs) paths.push_back(s.id);
                        playlist_manager_sync(paths.data(), (int)paths.size());
                        playlist_manager_set_index(0);
                    }
                } else {
                    int next = cur.netease_selected + 1;
                    if (next < (int)cur.netease_menu.size())
                        StateStore::instance().set_netease_selected(next);
                }
            } else {
                if (!cur.playlist.empty() && cur.selected_index < (int)cur.playlist.size() - 1)
                    StateStore::instance().set_selected_index(cur.selected_index + 1);
            }
            return true;

        case Action::MoveUp:
            if (cur.active_panel == 0) {
                if (cur.music_mode == MusicMode::Local) {
                    int prev = cur.group_index - 1;
                    if (prev < -1) return true; /* already at top (netease entry) */
                    if (prev >= 0) {
                        StateStore::instance().set_group_index(prev);
                        std::vector<const char*> paths;
                        for (auto &s : cur.groups[prev].songs) paths.push_back(s.id);
                        playlist_manager_sync(paths.data(), (int)paths.size());
                        playlist_manager_set_index(0);
                    } else {
                        StateStore::instance().set_group_index(-1);
                    }
                } else {
                    int prev = cur.netease_selected - 1;
                    if (prev >= -1)
                        StateStore::instance().set_netease_selected(prev);
                }
            } else {
                if (cur.selected_index > 0)
                    StateStore::instance().set_selected_index(cur.selected_index - 1);
            }
            return true;

        case Action::PlayPause:
            if (cur.playback_state == PlaybackState::Playing)
                event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
            else if (cur.playback_state == PlaybackState::Paused)
                event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
            else if (cur.playback_state == PlaybackState::Stopped) {
                /* resumed from stop — play selected song */
                if (cur.playlist.empty()) return true;
                int idx = cur.selected_index;
                playlist_manager_set_index(idx);
                const auto &sel = cur.playlist[idx];
                StateStore::instance().set_current_song(sel);
                event_bus_publish(EV_TRACK_CHANGED, NULL, 0);
                event_bus_publish(EV_PLAYBACK_START, (void*)(sel.id ? sel.id : ""),
                                  strlen(sel.id ? sel.id : "") + 1);
            }
            return true;

        case Action::PlaySelected:
            if (cur.active_panel == 0) {
                /* Left panel: mode switching or menu selection */
                if (cur.music_mode == MusicMode::Local && cur.group_index < 0) {
                    /* Switch to Netease mode */
                    StateStore::instance().set_music_mode(MusicMode::Netease);
                    StateStore::instance().set_active_panel(0);
                    StateStore::instance().set_group_index(-1);
                } else if (cur.music_mode == MusicMode::Netease && cur.netease_selected < 0) {
                    /* Switch back to Local mode */
                    StateStore::instance().set_music_mode(MusicMode::Local);
                    StateStore::instance().set_active_panel(0);
                    StateStore::instance().set_group_index(0);
                } else if (cur.music_mode == MusicMode::Netease && cur.netease_selected >= 0) {
                    /* Load netease menu item content into right panel */
                    int idx = cur.netease_selected;
                    if (idx < (int)cur.netease_menu.size()) {
                        int type = cur.netease_menu[idx].type;
                        const std::string &pl_id = cur.netease_menu[idx].id;

                        if (type == -1) {
                            /* Back to main netease menu: clear first so
                               set_music_mode reinitializes the default menu */
                            StateStore::instance().set_netease_menu({});
                            StateStore::instance().set_music_mode(MusicMode::Local);
                            StateStore::instance().set_music_mode(MusicMode::Netease);
                        } else if (type == 200) {
                            start_login();
                        } else if (type == 100) {
                            search_manager_clear();
                            StateStore::instance().set_search_active(true);
                            StateStore::instance().set_search_query("");
                        } else if (!pl_id.empty()) {
                            SearchResult sr;
                            int ret = netease_get_playlist_songs(pl_id.c_str(), &sr);
                            if (ret == 0 && sr.count > 0) {
                                fprintf(stderr, "\nDIAG: playlist loaded ret=%d count=%d\n", ret, sr.count);
                                std::vector<SongInfo> vec;
                                vec.reserve(sr.count);
                                for (int i = 0; i < sr.count; i++) {
                                    SongInfo copy = {};
                                    song_info_copy(&copy, &sr.songs[i]);
                                    vec.push_back(copy);
                                    song_info_free(&sr.songs[i]);
                                }
                                free(sr.songs);
                                StateStore::instance().set_playlist(vec, 0);
                                StateStore::instance().set_active_panel(1);
                            }
                        } else if (type >= 0 && type <= 1) {
                            SearchResult sr;
                            if (netease_load_menu(type, 200, &sr) == 0 && sr.count > 0) {
                                fprintf(stderr, "\nDIAG: load_menu type=%d count=%d\n", type, sr.count);
                                std::vector<SongInfo> vec;
                                vec.reserve(sr.count);
                                for (int i = 0; i < sr.count; i++) {
                                    SongInfo copy = {};
                                    song_info_copy(&copy, &sr.songs[i]);
                                    vec.push_back(copy);
                                    song_info_free(&sr.songs[i]);
                                }
                                free(sr.songs);
                                StateStore::instance().set_playlist(vec, 0);
                                StateStore::instance().set_active_panel(1);
                            }
                        } else if (type == 2 || type == 3) {
                            if (!netease_is_logged_in()) {
                                start_login();
                            } else {
                                NeteasePlaylistResult pr;
                                if (netease_get_playlists(&pr) == 0 && pr.count > 0) {
                                    std::vector<NeteaseMenuItem> items;
                                    items.push_back({"<< \u8FD4\u56DE", -1, ""});
                                    for (int i = 0; i < pr.count; i++) {
                                        /* type=2: created (subscribed=false), type=3: favorited (subscribed=true) */
                                        bool want = (type == 2) ? !pr.items[i].subscribed : pr.items[i].subscribed;
                                        if (!want) continue;
                                        char id_buf[32];
                                        snprintf(id_buf, sizeof(id_buf), "%llu", (unsigned long long)pr.items[i].id);
                                        items.push_back({pr.items[i].name, 1000, id_buf});
                                    }
                                    netease_playlist_result_free(&pr);
                                    StateStore::instance().set_netease_menu(items);
                                    StateStore::instance().set_netease_selected(0);
                                }
                            }
                        }
                    }
                }
                return true;
            }
            /* Right panel: play selected song */
            if (cur.playlist.empty()) return true;
            if (cur.active_panel == 1) {
                int idx = cur.selected_index;
                playlist_manager_set_index(idx);
                const auto &sel = cur.playlist[idx];
                const char *path = sel.id ? sel.id : "";
                fprintf(stderr, "\nDIAG_PLAY: id='%s' source='%s'\n",
                        path, sel.source ? sel.source : "(null)");
                StateStore::instance().set_current_song(sel);
                event_bus_publish(EV_PLAYBACK_START, (void*)path, strlen(path) + 1);
            }
            return true;

        case Action::NextTrack: {
            int next = playlist_manager_advance();
            if (next >= 0) {
                const char *path = playlist_manager_get_path(next);
                if (path) {
                    StateStore::instance().set_selected_index(next);
                    if (next < (int)cur.playlist.size()) {
                        StateStore::instance().set_current_song(cur.playlist[next]);
                        event_bus_publish(EV_TRACK_CHANGED, NULL, 0);
                    }
                    event_bus_publish(EV_PLAYBACK_START, (void*)path, strlen(path) + 1);
                }
            }
            return true;
        }

        case Action::PrevTrack: {
            int prev = playlist_manager_retreat();
            if (prev >= 0) {
                const char *path = playlist_manager_get_path(prev);
                if (path) {
                    StateStore::instance().set_selected_index(prev);
                    if (prev < (int)cur.playlist.size()) {
                        StateStore::instance().set_current_song(cur.playlist[prev]);
                        event_bus_publish(EV_TRACK_CHANGED, NULL, 0);
                    }
                    event_bus_publish(EV_PLAYBACK_START, (void*)path, strlen(path) + 1);
                }
            }
            return true;
        }

        case Action::VolumeUp: {
            int vol = audio_output_get_volume();
            if (vol >= 0) {
                vol = (vol + 5 > 100) ? 100 : vol + 5;
                event_bus_publish(EV_VOLUME_CHANGED, &vol, sizeof(vol));
            }
            return true;
        }

        case Action::VolumeDown: {
            int vol = audio_output_get_volume();
            if (vol >= 0) {
                vol = (vol - 5 < 0) ? 0 : vol - 5;
                event_bus_publish(EV_VOLUME_CHANGED, &vol, sizeof(vol));
            }
            return true;
        }

        case Action::SeekForward: {
            if (cur.playback_state != PlaybackState::Stopped) {
                int step = config_get_int(config_global(), "playback.seek_step_sec", 5);
                int target = cur.current_time_sec + step;
                if (target > cur.total_time_sec) target = cur.total_time_sec;
                event_bus_publish(EV_BUFFERING_UPDATE, &target, sizeof(target));
            }
            return true;
        }

        case Action::SeekBackward: {
            if (cur.playback_state != PlaybackState::Stopped) {
                int step = config_get_int(config_global(), "playback.seek_step_sec", 5);
                int target = cur.current_time_sec - step;
                if (target < 0) target = 0;
                event_bus_publish(EV_BUFFERING_UPDATE, &target, sizeof(target));
            }
            return true;
        }

        case Action::Stop:
            if (cur.playback_state != PlaybackState::Stopped) {
                event_bus_publish(EV_PLAYBACK_STOP, NULL, 0);
            }
            return true;

        case Action::ToggleMute: {
            int muted_val = cur.muted ? 0 : 1;
            event_bus_publish(EV_MUTE_CHANGED, &muted_val, sizeof(muted_val));
            return true;
        }

        case Action::OpenSearch: {
            bool was_active = cur.search_active;
            if (was_active) {
                search_manager_clear();
                StateStore::instance().set_search_active(false);
                StateStore::instance().set_search_query("");
            } else {
                /* show empty search bar — user types to start searching */
                search_manager_clear();
                StateStore::instance().set_search_active(true);
                StateStore::instance().set_search_query("");
            }
            return true;
        }

        case Action::ShowHelp:
            StateStore::instance().set_show_help(!cur.show_help);
            return true;

        case Action::CycleLoop: {
            int next = ((int)cur.loop_mode + 1) % 3;
            event_bus_publish(EV_PLAYLIST_CHANGED, &next, sizeof(next));
            return true;
        }

        default:
            return false;
        }
    });

    screen.Loop(component);

    LOG_INFO("Shutting down");
    timer_active.store(false);
    refresh_timer.join();
    event_bus_publish(EV_APP_SHUTDOWN, NULL, 0);
    playback_coordinator_shutdown();
    music_source_manager_shutdown();
    free_ns_cache();
    event_bus_shutdown();
    config_free(cfg);
    log_shutdown();
    return 0;
}
