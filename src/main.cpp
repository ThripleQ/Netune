#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "infra/log.h"
#include "infra/config.h"
#include "core/event_bus.h"
}

using namespace ftxui;

static volatile bool g_running = true;

static void on_signal(int sig) {
    (void)sig;
    g_running = false;
}

/* ── Event handlers (stubs for Phase 0) ────────────── */
static void on_startup(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    LOG_INFO("App startup event received");
}

static void on_shutdown(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    LOG_INFO("App shutdown event received");
}

int main(int argc, char **argv) {
    /* ── Infrastructure ────────────────────────────── */
    log_init(NULL);  /* stderr only for now */
    LOG_INFO("LMusic v2.0.0 starting");

    /* quick arg check */
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "-h") == 0)) {
        printf("LMusic v2.0.0 — terminal music player\n");
        printf("Usage: %s [config.json]\n", argv[0]);
        return 0;
    }

    /* load config */
    Config *cfg = NULL;
    const char *cfg_path = "data/config.json";
    if (argc > 1) cfg_path = argv[1];
    cfg = config_load(cfg_path);
    if (!cfg) {
        LOG_WARN("No config loaded, using defaults");
    }

    /* ── Event bus ──────────────────────────────────── */
    if (event_bus_init() != 0) {
        LOG_ERROR("Event bus init failed");
        config_free(cfg);
        return 1;
    }

    event_bus_subscribe(EV_APP_STARTUP, on_startup, NULL);
    event_bus_subscribe(EV_APP_SHUTDOWN, on_shutdown, NULL);
    event_bus_publish(EV_APP_STARTUP, NULL, 0);
    event_bus_poll();

    /* ── Signal handlers ────────────────────────────── */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* ── FTXUI screen ────────────────────────────────── */
    auto screen = ScreenInteractive::Fullscreen();

    /* placeholder UI */
    std::string status = "LMusic v2.0.0 — Phase 0";
    auto renderer = Renderer([&] {
        return vbox({
            text(" LMusic v2.0.0 ") | bold | center | border,
            separator(),
            text("  Status: " + status) | dim,
            text("  Press q or Esc to quit"),
        }) | border | center;
    });

    /* key bindings */
    renderer |= CatchEvent([&](ftxui::Event event) -> bool {
        if (event == ftxui::Event::Character('q') ||
            event == ftxui::Event::Escape) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    LOG_INFO("Entering main loop");
    screen.Loop(renderer);

    /* ── Shutdown ───────────────────────────────────── */
    LOG_INFO("Shutting down");
    event_bus_publish(EV_APP_SHUTDOWN, NULL, 0);
    event_bus_poll();
    event_bus_shutdown();
    config_free(cfg);
    log_shutdown();

    return 0;
}
