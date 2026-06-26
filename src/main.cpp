#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

extern "C" {
#include "infra/log.h"
#include "infra/config.h"
#include "core/event_bus.h"
#include "core/playback_coordinator.h"
#include "core/music_source.h"
#include "plugins/music_sources/local/local_source.h"
}

using namespace ftxui;

/* ── Global state bridge (C++ side) ─────────────────── */
enum class PlayState { Stopped, Playing, Paused };

static struct {
    PlayState play_state = PlayState::Stopped;
    std::string current_path;
    std::string current_title;
    double      progress = 0.0;
    int         time_sec = 0;
    int         total_sec = 0;
    std::vector<SongInfo> playlist;
    int         selected = 0;
} g_state;

static volatile bool g_running = true;

/* ── Signal handler ──────────────────────────────── */
static void on_signal(int sig) {
    (void)sig;
    g_running = false;
}

/* ── Event bus callbacks ──────────────────────────── */
static void ev_progress(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int[2])) {
        int *p = (int*)ev->data;
        g_state.time_sec = p[0];
        g_state.total_sec = p[1];
        g_state.progress = (g_state.total_sec > 0)
            ? (double)g_state.time_sec / g_state.total_sec : 0.0;
    }
}

static void ev_playback_start(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    g_state.play_state = PlayState::Playing;
}

static void ev_playback_pause(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    g_state.play_state = PlayState::Paused;
}

static void ev_playback_resume(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    g_state.play_state = PlayState::Playing;
}

static void ev_playback_stop(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    g_state.play_state = PlayState::Stopped;
    g_state.progress = 0.0;
    g_state.time_sec = 0;
}

static void ev_playback_finish(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    g_state.play_state = PlayState::Stopped;
}

/* ── Main ────────────────────────────────────────────── */
int main(int argc, char **argv) {
    /* logging */
    log_init(NULL);
    LOG_INFO("LMusic v2.0.0 starting");

    /* quick --help */
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

    /* subscribe UI callbacks */
    event_bus_subscribe(EV_PROGRESS_UPDATE, ev_progress, NULL);
    event_bus_subscribe(EV_PLAYBACK_START, ev_playback_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE, ev_playback_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME, ev_playback_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP, ev_playback_stop, NULL);
    event_bus_subscribe(EV_PLAYBACK_FINISH, ev_playback_finish, NULL);

    /* register plugins */
    local_source_register();

    /* playback coordinator */
    playback_coordinator_init();

    /* startup event */
    event_bus_publish(EV_APP_STARTUP, NULL, 0);
    event_bus_poll();

    /* signals */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* ── UI ────────────────────────────────────── */
    auto screen = ScreenInteractive::Fullscreen();

    /* build the renderer component */
    auto component = Renderer([&]() -> Element {
        /* progress text */
        char buf[64];
        int m = g_state.time_sec / 60, s = g_state.time_sec % 60;
        int tm = g_state.total_sec / 60, ts = g_state.total_sec % 60;
        snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d", m, s, tm, ts);
        std::string progress_str = buf;

        /* state label */
        std::string state_str;
        switch (g_state.play_state) {
        case PlayState::Playing: state_str = "\u25B6 Playing"; break;
        case PlayState::Paused:  state_str = "\u23F8 Paused";  break;
        default:                 state_str = "\u25A0 Stopped"; break;
        }

        /* title */
        std::string title = g_state.current_title.empty()
            ? "No track loaded" : g_state.current_title;

        /* playlist entries */
        Elements entries;
        for (size_t i = 0; i < g_state.playlist.size(); i++) {
            auto &s = g_state.playlist[i];
            std::string label = s.title ? s.title : "(unknown)";
            if (s.artist && s.artist[0])
                label = std::string(s.artist) + " - " + label;
            if ((int)i == g_state.selected)
                entries.push_back(text(label) | bold | inverted);
            else
                entries.push_back(text(label));
        }

        /* assemble layout */
        return vbox(Elements{
            text(" LMusic v2.0.0 ") | bold | center,
            separator(),
            text(" " + title) | bold,
            text(" " + state_str + "  |  " + progress_str) | dim,
            gauge(g_state.progress),
            separator(),
            text(" Playlist:") | bold,
            vbox(std::move(entries)) | yframe | flex,
            separator(),
            text(" [s]can dir  [Enter]play  [Space]pause  [j/k]nav  [q]quit") | dim,
        }) | border;
    });

    /* key bindings */
    component |= CatchEvent([&](ftxui::Event event) -> bool {
        if (event == ftxui::Event::Character('q') ||
            event == ftxui::Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }

        /* scan directory */
        if (event == ftxui::Event::Character('s')) {
            /* try common music dirs */
            const char *dirs[] = {
                getenv("HOME"),
                "/home/liu/Music",
                "/run/media/liu",
                NULL
            };
            bool found = false;
            for (int i = 0; dirs[i]; i++) {
                MusicSource *src = local_source_create();
                SearchResult result;
                memset(&result, 0, sizeof(result));
                if (src->search(dirs[i], 0, 0, &result) == 0 && result.count > 0) {
                    /* transfer songs to playlist */
                    for (int j = 0; j < result.count; j++) {
                        SongInfo copy;
                        song_info_copy(&copy, &result.songs[j]);
                        g_state.playlist.push_back(copy);
                        song_info_free(&result.songs[j]);
                    }
                    free(result.songs);
                    found = true;
                    LOG_INFO("Scanned %s: %d songs", dirs[i], result.count);
                    break;
                }
            }
            if (!found)
                LOG_WARN("No music files found in standard dirs");
            return true;
        }

        /* play / pause */
        if (event == ftxui::Event::Character('\r') ||
            event == ftxui::Event::Character(' ')) {
            if (g_state.playlist.empty()) return true;

            if (g_state.play_state == PlayState::Playing) {
                event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
            } else if (g_state.play_state == PlayState::Paused) {
                event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
            } else {
                const SongInfo &s = g_state.playlist[g_state.selected];
                const char *path = s.id ? s.id : "";
                g_state.current_path = path;
                g_state.current_title = s.title ? s.title : path;
                event_bus_publish(EV_PLAYBACK_START,
                                  (void*)path, strlen(path) + 1);
            }
            return true;
        }

        /* navigation */
        if (event == ftxui::Event::Character('j') ||
            event == ftxui::Event::ArrowDown) {
            if (!g_state.playlist.empty() &&
                g_state.selected < (int)g_state.playlist.size() - 1)
                g_state.selected++;
            return true;
        }
        if (event == ftxui::Event::Character('k') ||
            event == ftxui::Event::ArrowUp) {
            if (g_state.selected > 0)
                g_state.selected--;
            return true;
        }

        return false;
    });

    screen.Loop(component);

    /* ── Shutdown ───────────────────────────────── */
    LOG_INFO("Shutting down");
    event_bus_publish(EV_APP_SHUTDOWN, NULL, 0);
    playback_coordinator_shutdown();
    event_bus_shutdown();
    config_free(cfg);
    log_shutdown();

    return 0;
}
