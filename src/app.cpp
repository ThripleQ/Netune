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

extern "C" {
#include "infra/log.h"
#include "infra/config.h"
#include "core/event_bus.h"
#include "core/playback_coordinator.h"
#include "core/music_source_manager.h"
#include "core/music_source.h"
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
    if (ev->data_size == sizeof(int[2])) {
        int *p = (int*)ev->data;
        double prog = (p[1] > 0) ? (double)p[0] / p[1] : 0.0;
        StateStore::instance().set_progress(prog, p[0], p[1]);
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
    StateStore::instance().set_playback_state(PlaybackState::Stopped);
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

    /* event bus */
    event_bus_init();

    /* subscribe → StateStore */
    event_bus_subscribe(EV_PROGRESS_UPDATE,   ev_progress, NULL);
    event_bus_subscribe(EV_PLAYBACK_START,    ev_playback_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE,    ev_playback_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME,   ev_playback_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP,     ev_playback_stop, NULL);
    event_bus_subscribe(EV_PLAYBACK_FINISH,   ev_playback_finish, NULL);

    /* music sources */
    music_source_manager_init();
    local_source_register();

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
    auto &state = StateStore::instance();

    auto component = Renderer([&]() -> Element {
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
            text(" [s]can dir  [Enter]play  [Space]pause  [j/k]nav  [q]quit") | dim,
        }) | border;
    });

    component |= CatchEvent([&](ftxui::Event event) -> bool {
        if (event == ftxui::Event::Character('q') ||
            event == ftxui::Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }

        /* scan directory */
        if (event == ftxui::Event::Character('s')) {
            const char *dirs[] = {
                getenv("HOME"),
                "/home/liu/Music",
                "/run/media/liu",
                NULL
            };
            bool found = false;
            for (int i = 0; dirs[i]; i++) {
                SearchResult result;
                memset(&result, 0, sizeof(result));
                if (music_source_search("local", dirs[i], 0, 0, &result) == 0
                    && result.count > 0) {
                    std::vector<SongInfo> pl;
                    for (int j = 0; j < result.count; j++) {
                        SongInfo copy;
                        song_info_copy(&copy, &result.songs[j]);
                        pl.push_back(copy);
                        song_info_free(&result.songs[j]);
                    }
                    free(result.songs);
                    StateStore::instance().set_playlist(pl, 0);
                    LOG_INFO("Scanned %s: %zu songs", dirs[i], pl.size());
                    found = true;
                    break;
                }
            }
            if (!found)
                LOG_WARN("No music files found");
            return true;
        }

        /* play / pause */
        if (event == ftxui::Event::Character('\r') ||
            event == ftxui::Event::Character(' ')) {
            const AppState &s = state.state();
            if (s.playlist.empty()) return true;

            if (s.playback_state == PlaybackState::Playing) {
                event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
            } else if (s.playback_state == PlaybackState::Paused) {
                event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
            } else {
                const SongInfo &song = s.playlist[s.selected_index];
                const char *path = song.id ? song.id : "";
                StateStore::instance().set_current_song(song);
                event_bus_publish(EV_PLAYBACK_START,
                                  (void*)path, strlen(path) + 1);
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

        return false;
    });

    screen.Loop(component);

    /* ── Shutdown ───────────────────────────────── */
    LOG_INFO("Shutting down");
    event_bus_publish(EV_APP_SHUTDOWN, NULL, 0);
    playback_coordinator_shutdown();
    music_source_manager_shutdown();
    event_bus_shutdown();
    config_free(cfg);
    log_shutdown();

    return 0;
}
