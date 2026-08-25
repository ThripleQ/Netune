#ifndef NETUNE_APP_BOOTSTRAP_H
#define NETUNE_APP_BOOTSTRAP_H

#ifdef __cplusplus
extern "C" {
#endif

struct Config;
struct threadpool;

/* One-time startup: initialize logging, config (with music_sources
 * backfill), cache, search manager, event bus and the worker thread
 * pool, and start the download queue. Must be called before any UI /
 * playback activity. Returns the loaded global config (may be NULL if
 * config.json is missing/unreadable). If out_pool is non-NULL, receives
 * the created thread pool (the caller owns and destroys it). */
struct Config *app_bootstrap(struct threadpool **out_pool);

#ifdef __cplusplus
}
#endif

#endif /* NETUNE_APP_BOOTSTRAP_H */
