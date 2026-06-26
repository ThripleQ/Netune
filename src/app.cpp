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
#include "plugins/music_sources/local/local_source.h"
}

#include "ui/state_store.h"

using namespace ftxui;

/* ── Signal handler ──────────────────────────────── */
static volatile bool g_running = true;

static void on_signal(int sig) {
    (void)sig;
    g_running = false;
}

/* ── Event bus → StateStore bridge ────────────────── */
static void ev_progress(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int[3])) {
        int *p = (int*)ev->data;
        int frame_pos   = p[0];   /* exact PCM frame */
        int total_frames = p[1];
        int samplerate   = p[2];
        double prog = (total_frames > 0)
            ? (double)frame_pos / total_frames : 0.0;
        int time_sec  = (samplerate > 0) ? frame_pos / samplerate : 0;
        int total_sec = (samplerate > 0 && total_frames > 0)
            ? total_frames / samplerate : 0;
        StateStore::instance().set_progress(prog, time_sec, total_sec);
    }
}

static void ev_playback_start(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    StateStore::instance().set_playback_state(PlaybackState::Playing);
}

static void ev_playback_pause(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    StateStore::instance().set_playback_state(PlaybackState::Paused);
}

static void ev_playback_resume(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    StateStore::instance().set_playback_state(PlaybackState::Playing);
}

static void ev_playback_stop(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    StateStore::instance().set_playback_state(PlaybackState::Stopped);
    StateStore::instance().set_progress(0.0, 0, 0);
}

static void ev_playback_finish(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    /* Check for auto-advance based on loop mode */
    int next = playlist_manager_advance();
    if (next >= 0) {
        const SongInfo *song = playlist_manager_get(next);
        if (song && song->id) {
            StateStore::instance().set_selected_index(next);
            StateStore::instance().set_current_song(*song);
            event_bus_publish(EV_PLAYBACK_START,
                              (void*)song->id, strlen(song->id) + 1);
            return;
        }
    }
    StateStore::instance().set_playback_state(PlaybackState::Stopped);
}

static void ev_playback_error(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    LOG_WARN("Playback error, resetting state");
    StateStore::instance().set_playback_state(PlaybackState::Stopped);
    StateStore::instance().set_progress(0.0, 0, 0);
}

/* ── App entry point ─────────────────────────────────── */
int run_app(int argc, char **argv) {
    log_init(NULL);
    LOG_INFO("LMusic v2.0.0 starting");

    /* --help */
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "-h") == 0)) {
        printf("LMusic v2.0.0 — Terminal music player\n");
        printf("Usage: %s [config.json]\n", argv[0]);
        return 0;
    }

    /* config */
    Config *cfg = NULL;
    const char *cfg_path = "data/config.json";
    if (argc > 1) cfg_path = argv[1];
    cfg = config_load(cfg_path);
    if (!cfg) LOG_WARN("No config loaded, using defaults");
    config_set_global(cfg);  /* make accessible everywhere */

    /* event bus */
    event_bus_init();

    /* subscribe → StateStore */
    event_bus_subscribe(EV_PROGRESS_UPDATE,   ev_progress, NULL);
    event_bus_subscribe(EV_PLAYBACK_START,    ev_playback_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE,    ev_playback_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME,   ev_playback_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP,     ev_playback_stop, NULL);
    event_bus_subscribe(EV_PLAYBACK_FINISH,   ev_playback_finish, NULL);
    event_bus_subscribe(EV_PLAYBACK_ERROR,    ev_playback_error, NULL);

    /* load config values into state */
    if (cfg) {
        int vol = config_get_int(cfg, "audio.volume", -1);
        if (vol >= 0 && vol <= 100)
            StateStore::instance().set_volume(vol);
        int loop = config_get_int(cfg, "playback.loop_mode", 0);
        if (loop >= 0 && loop <= 2) {
            playlist_manager_set_loop_mode(loop);
            StateStore::instance().set_loop_mode((LoopMode)loop);
        }
    }

    /* music sources */
    music_source_manager_init();
    local_source_register();

    /* auto-scan: read configured dirs and build initial playlist */
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
        std::vector<SongInfo> pl;
        for (auto &dir : scan_dirs) {
            SearchResult result;
            memset(&result, 0, sizeof(result));
            if (music_source_search("local", dir.c_str(), 0, 0, &result) == 0) {
                for (int j = 0; j < result.count; j++) {
                    SongInfo copy;
                    song_info_copy(&copy, &result.songs[j]);
                    pl.push_back(copy);
                    song_info_free(&result.songs[j]);
                }
                free(result.songs);
            }
        }
        if (!pl.empty()) {
            size_t n = pl.size();
            /* Transfer ownership to playlist_manager */
            SongInfo *arr = (SongInfo*)malloc(n * sizeof(SongInfo));
            for (size_t i = 0; i < n; i++) {
                song_info_copy(&arr[i], &pl[i]);
                song_info_free(&pl[i]);
            }
            pl.clear();
            playlist_manager_set_playlist(arr, (int)n, "local");
            /* Mirror to StateStore for UI */
            std::vector<SongInfo> ui_pl;
            for (size_t i = 0; i < n; i++) {
                SongInfo copy;
                song_info_copy(&copy, &arr[i]);
                ui_pl.push_back(copy);
            }
            StateStore::instance().set_playlist(ui_pl, 0);
            LOG_INFO("Auto-scanned %zu songs from config", n);
        }
    }

    /* playback coordinator */
    playback_coordinator_init();

    /* startup event */
    event_bus_publish(EV_APP_STARTUP, NULL, 0);
    event_bus_poll();

    /* signals */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* ── FTXUI UI ───────────────────────────────── */
    auto screen = ScreenInteractive::Fullscreen();

    /* UI refresh timer: FTXUI only redraws on terminal events.
       Without this, progress bar freezes and event_bus_poll()
       never runs during playback. */
    std::atomic<bool> timer_active{true};
    std::thread refresh_timer([&]() {
        while (timer_active.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); /* ~60fps */
            screen.RequestAnimationFrame();
        }
    });

    auto &state = StateStore::instance();

    /* Poll event bus every render frame — essential for key events to reach
       the playback coordinator thread. */
    auto component = Renderer([&]() -> Element {
        event_bus_poll();
        const AppState &s = state.state();

        /* state label */
        std::string state_str;
        switch (s.playback_state) {
        case PlaybackState::Playing: state_str = "\u25B6 Playing"; break;
        case PlaybackState::Paused:  state_str = "\u23F8 Paused";  break;
        default:                     state_str = "\u25A0 Stopped"; break;
        }

        /* time */
        char buf[64];
        int m = s.current_time_sec / 60, sec = s.current_time_sec % 60;
        int tm = s.total_time_sec / 60, tsec = s.total_time_sec % 60;
        snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d", m, sec, tm, tsec);
        std::string time_str = buf;

        /* title */
        std::string title = s.current_song.title
            ? std::string(s.current_song.title) : "No track loaded";

        /* playlist */
        Elements entries;
        for (size_t i = 0; i < s.playlist.size(); i++) {
            std::string label = s.playlist[i].title
                ? s.playlist[i].title : "(unknown)";
            if (s.playlist[i].artist && s.playlist[i].artist[0])
                label = std::string(s.playlist[i].artist) + " - " + label;
            if ((int)i == s.selected_index)
                entries.push_back(text(label) | bold | inverted);
            else
                entries.push_back(text(label));
        }

        return vbox(Elements{
            text(" LMusic v2.0.0 ") | bold | center,
            separator(),
            text(" " + title) | bold,
            text(" " + state_str + "  |  " + time_str) | dim,
            gauge(s.progress),
            separator(),
            text(" Playlist:") | bold,
            vbox(std::move(entries)) | yframe | flex,
            separator(),
            text(" [Enter]play  [Space]pause  [j/k]nav  [n/p]track  [←/→]seek  [q]quit") | dim,
        }) | border;
    });

    component |= CatchEvent([&](ftxui::Event event) -> bool {
        if (event == ftxui::Event::Character('q') ||
            event == ftxui::Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }

        /* Enter: play selected track (always plays the selected song) */
        if (event == ftxui::Event::Return) {
            const AppState &s = state.state();
            if (s.playlist.empty()) return true;
            int idx = s.selected_index;
            playlist_manager_set_index(idx);
            const SongInfo &sel = s.playlist[idx];
            const char *path = sel.id ? sel.id : "";
            StateStore::instance().set_current_song(sel);
            event_bus_publish(EV_PLAYBACK_START,
                              (void*)path, strlen(path) + 1);
            return true;
        }

        /* Space: pause/resume current playback only */
        if (event == ftxui::Event::Character(' ')) {
            const AppState &s = state.state();
            if (s.playback_state == PlaybackState::Playing) {
                event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
            } else if (s.playback_state == PlaybackState::Paused) {
                event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
            }
            return true;
        }

        /* navigation */
        if (event == ftxui::Event::Character('j') ||
            event == ftxui::Event::ArrowDown) {
            const AppState &s = state.state();
            if (!s.playlist.empty() &&
                s.selected_index < (int)s.playlist.size() - 1)
                StateStore::instance().set_selected_index(s.selected_index + 1);
            return true;
        }
        if (event == ftxui::Event::Character('k') ||
            event == ftxui::Event::ArrowUp) {
            const AppState &s = state.state();
            if (s.selected_index > 0)
                StateStore::instance().set_selected_index(s.selected_index - 1);
            return true;
        }

        /* seek backward / forward */
        if (event == ftxui::Event::ArrowLeft) {
            const AppState &s = state.state();
            if (s.playback_state != PlaybackState::Stopped) {
                int step = config_get_int(config_global(),
                                         "playback.seek_step_sec", 5);
                int target = s.current_time_sec - step;
                if (target < 0) target = 0;
                event_bus_publish(EV_BUFFERING_UPDATE, &target, sizeof(target));
            }
            return true;
        }
        if (event == ftxui::Event::ArrowRight) {
            const AppState &s = state.state();
            if (s.playback_state != PlaybackState::Stopped) {
                int step = config_get_int(config_global(),
                                         "playback.seek_step_sec", 5);
                int target = s.current_time_sec + step;
                if (target > s.total_time_sec) target = s.total_time_sec;
                event_bus_publish(EV_BUFFERING_UPDATE, &target, sizeof(target));
            }
            return true;
        }

        /* next / prev track — route through playlist_manager */
        if (event == ftxui::Event::Character('n')) {
            int next = playlist_manager_advance();
            if (next >= 0) {
                const SongInfo *song = playlist_manager_get(next);
                if (song && song->id) {
                    StateStore::instance().set_selected_index(next);
                    StateStore::instance().set_current_song(*song);
                    event_bus_publish(EV_PLAYBACK_START,
                                      (void*)song->id, strlen(song->id) + 1);
                }
            }
            return true;
        }
        if (event == ftxui::Event::Character('p')) {
            int prev = playlist_manager_retreat();
            if (prev >= 0) {
                const SongInfo *song = playlist_manager_get(prev);
                if (song && song->id) {
                    StateStore::instance().set_selected_index(prev);
                    StateStore::instance().set_current_song(*song);
                    event_bus_publish(EV_PLAYBACK_START,
                                      (void*)song->id, strlen(song->id) + 1);
                }
            }
            return true;
        }

        return false;
    });

    screen.Loop(component);

    /* ── Shutdown ───────────────────────────────── */
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
