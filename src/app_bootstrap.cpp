/* app_bootstrap.cpp — one-time startup wiring: logging, config (with
 * music_sources backfill), cache, event bus, thread pool and the
 * download queue.
 *
 * Kept separate from app.cpp so the big UI run_app() only deals with
 * rendering and event bridging.  This module owns no UI and no state
 * beyond what it initializes.
 */
#include "app_bootstrap.h"
#include "infra/config_paths.h"
#include "infra/config.h"
#include "infra/log.h"
#include "infra/thread_pool.h"
#include "infra/defaults.h"
#include "core/cache_manager.h"
#include "core/event_bus.h"
#include "core/search_manager.h"
#include "compat/utf8.h"
#include "ui/download_queue.h"

#include <stdio.h>

#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

/* app.cpp owns the thread pool global; it is handed back via out_pool. */

Config *app_bootstrap(threadpool_t **out_pool) {
    /* ── Log ────────────────────────────────────────── */
    const char *log_path = netune_xdg_dir("XDG_CACHE_HOME", "netune.log");
    netune_ensure_dir(log_path);
    log_init(log_path);
    LOG_INFO("Netune v2.0.0 starting");

    /* Probe terminal cell size (before FTXUI takes over the terminal)
       so the character cover renderer can keep aspect ratio on any font */
    cover_cell_probe();

    /* ── Ensure default data tree exists (XDG_CONFIG_HOME/netune/data/) ── */
    /* Rebuilds config.json / themes / layouts / keybindings if missing.
       No scanning, no fallback lookups elsewhere. */
    netune_ensure_default_data_tree();

    /* ── Config (under data/) ── */
    char cfg_buf[2048];
    snprintf(cfg_buf, sizeof(cfg_buf), "%s" PATH_SEP "config.json",
             netune_data_root());
    Config *cfg = config_load(cfg_buf);
    if (!cfg) LOG_WARN("No config loaded, using defaults");
    config_set_global(cfg);

    /* Backfill missing core sections (older configs / partial writes):
       without music_sources the local source silently disables itself. */
    if (cfg && !config_has(cfg, "music_sources")) {
        LOG_WARN("config.json missing 'music_sources' — backfilling defaults");
        config_set_str(cfg, "music_sources.local.enabled", "true");
        config_set_str(cfg, "music_sources.netease.enabled", "true");
        config_save(cfg);
    }

    /* ── Cache (XDG_CACHE_HOME) ─────────────────────── */
    const char *cache_dir = netune_xdg_dir("XDG_CACHE_HOME", NULL);
    netune_ensure_dir(cache_dir);
    mkdir_utf8(cache_dir);
    cache_init(cache_dir);
    search_manager_init();

    event_bus_init();

    threadpool_t *pool = threadpool_create(8);
    if (!pool)
        LOG_WARN("Failed to create thread pool, cover art will not load");
    if (out_pool) *out_pool = pool;

    DownloadQueue::instance().start();

    return cfg;
}
