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
#include "plugins/music_sources/local/local_source.h"
}

#include "ui/state_store.h"
#include "ui/keybindings.h"
#include "ui/components/top_bar.h"
#include "ui/components/status_bar.h"
#include "ui/components/group_list.h"
#include "ui/components/song_list.h"
#include "ui/components/player_controls.h"
#include "ui/components/help_screen.h"
#include "ui/theme.h"
#include "ui/layout_engine.h"

using namespace ftxui;

/* ── Global keybinding manager ──────────────────────── */
static KeybindingManager g_keybindings;

static volatile bool g_running = true;

static void on_signal(int sig) { (void)sig; g_running = false; }

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
            if (next < (int)st.playlist.size())
                StateStore::instance().set_current_song(st.playlist[next]);
            event_bus_publish(EV_PLAYBACK_START, (void*)path, strlen(path) + 1);
            return;
        }
    }
    StateStore::instance().set_playback_state(PlaybackState::Stopped);
}

static void on_switch_group(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    /* When group changes: sync the new group's paths to backend */
    const auto &st = StateStore::instance().state();
    int gi = st.group_index;
    if (gi < 0 || gi >= (int)st.groups.size()) return;
    std::vector<const char*> paths;
    for (auto &s : st.groups[gi].songs) paths.push_back(s.id);
    playlist_manager_sync(paths.data(), (int)paths.size());
    playlist_manager_set_index(st.selected_index);
}
/* ───────────────────────────────────────────────────── */

int run_app(int argc, char **argv) {
    log_init(NULL);
    LOG_INFO("LMusic v2.0.0 starting");

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("LMusic v2.0.0 — Terminal music player\nUsage: %s [config.json]\n", argv[0]);
        return 0;
    }

    Config *cfg = NULL;
    const char *cfg_path = "data/config.json";
    if (argc > 1) cfg_path = argv[1];
    cfg = config_load(cfg_path);
    if (!cfg) LOG_WARN("No config loaded, using defaults");
    config_set_global(cfg);

    event_bus_init();

    event_bus_subscribe(EV_PROGRESS_UPDATE,   ev_progress, NULL);
    event_bus_subscribe(EV_PLAYBACK_START,    ev_playback_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE,    ev_playback_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME,   ev_playback_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP,     ev_playback_stop, NULL);
    event_bus_subscribe(EV_PLAYBACK_FINISH,   ev_playback_finish, NULL);
    event_bus_subscribe(EV_PLAYBACK_ERROR,    ev_playback_error, NULL);
    /* group switching handled synchronously in set_group_index */

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
            on_switch_group(NULL, NULL);
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

        Element main = vbox(Elements{
            layout_engine.build(s) | flex,
        });

        if (s.show_help) {
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
        if (event.is_character()) {
            ev_key = event.character();
        } else if (event == ftxui::Event::Tab) { ev_key = "tab"; }
        else if (event == ftxui::Event::Return) { ev_key = "enter"; }
        else if (event == ftxui::Event::Escape) { ev_key = "escape"; }
        else if (event == ftxui::Event::ArrowUp) { ev_key = "up"; }
        else if (event == ftxui::Event::ArrowDown) { ev_key = "down"; }
        else if (event == ftxui::Event::ArrowLeft) { ev_key = "left"; }
        else if (event == ftxui::Event::ArrowRight) { ev_key = "right"; }
        if (ev_key.empty()) return false;

        auto action = g_keybindings.lookup(ev_key);
        if (!action.has_value()) return false;

        const AppState &cur = state.state();

        switch (action.value()) {

        case Action::Quit:
            screen.ExitLoopClosure()();
            return true;

        case Action::PanelSwitch:
            StateStore::instance().set_active_panel(cur.active_panel ? 0 : 1);
            return true;

        case Action::MoveDown:
            if (cur.active_panel == 0) {
                int next_grp = cur.group_index + 1;
                if (next_grp < (int)cur.groups.size()) {
                    StateStore::instance().set_group_index(next_grp);
                    std::vector<const char*> paths;
                    for (auto &s : cur.groups[next_grp].songs) paths.push_back(s.id);
                    playlist_manager_sync(paths.data(), (int)paths.size());
                    playlist_manager_set_index(0);
                }
            } else {
                if (!cur.playlist.empty() && cur.selected_index < (int)cur.playlist.size() - 1)
                    StateStore::instance().set_selected_index(cur.selected_index + 1);
            }
            return true;

        case Action::MoveUp:
            if (cur.active_panel == 0) {
                int next_grp = cur.group_index - 1;
                if (next_grp >= 0) {
                    StateStore::instance().set_group_index(next_grp);
                    std::vector<const char*> paths;
                    for (auto &s : cur.groups[next_grp].songs) paths.push_back(s.id);
                    playlist_manager_sync(paths.data(), (int)paths.size());
                    playlist_manager_set_index(0);
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
            return true;

        case Action::PlaySelected:
            if (cur.playlist.empty()) return true;
            if (cur.active_panel == 1) {
                int idx = cur.selected_index;
                playlist_manager_set_index(idx);
                const auto &sel = cur.playlist[idx];
                const char *path = sel.id ? sel.id : "";
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
                    if (next < (int)cur.playlist.size())
                        StateStore::instance().set_current_song(cur.playlist[next]);
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
                    if (prev < (int)cur.playlist.size())
                        StateStore::instance().set_current_song(cur.playlist[prev]);
                    event_bus_publish(EV_PLAYBACK_START, (void*)path, strlen(path) + 1);
                }
            }
            return true;
        }

        case Action::VolumeUp: {
            int vol = audio_output_get_volume();
            if (vol >= 0) {
                vol = (vol + 5 > 100) ? 100 : vol + 5;
                if (audio_output_set_volume(vol) == 0)
                    StateStore::instance().set_volume(vol);
            }
            return true;
        }

        case Action::VolumeDown: {
            int vol = audio_output_get_volume();
            if (vol >= 0) {
                vol = (vol - 5 < 0) ? 0 : vol - 5;
                if (audio_output_set_volume(vol) == 0)
                    StateStore::instance().set_volume(vol);
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
            bool new_muted = !cur.muted;
            int vol = audio_output_get_volume();
            if (vol >= 0) {
                int target = new_muted ? 0 : cur.volume;
                if (target == 0) target = cur.volume; /* restore volume on unmute */
                if (target <= 0) target = 80;
                audio_output_set_volume(target);
            }
            StateStore::instance().set_muted(new_muted);
            StateStore::instance().set_volume(new_muted ? 0 : cur.volume);
            return true;
        }

        case Action::ShowHelp:
            StateStore::instance().set_show_help(!cur.show_help);
            return true;

        case Action::CycleLoop: {
            int next = ((int)cur.loop_mode + 1) % 3;
            playlist_manager_set_loop_mode(next);
            StateStore::instance().set_loop_mode((LoopMode)next);
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
    event_bus_shutdown();
    config_free(cfg);
    log_shutdown();
    return 0;
}
