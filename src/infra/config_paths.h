/* config_paths.h — canonical runtime paths, shared by netune and netune-config.
 *
 * Single source of truth for where the app stores its data:
 *   data root   : XDG_CONFIG_HOME/netune/data  (APPDATA\netune\data on Windows)
 *   cache / log : XDG_CACHE_HOME/netune        (LOCALAPPDATA\netune on Windows)
 *
 * This module exists so the main player and the config tool can never
 * disagree about paths.  Before it existed, netune_config.cpp carried a
 * copy of app.cpp's xdg_data_root() (marked "mirrors app.cpp") which had
 * already drifted — it was missing the Windows APPDATA branch.
 */
#ifndef NETUNE_CONFIG_PATHS_H
#define NETUNE_CONFIG_PATHS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Directory holding ALL runtime-editable resources (config.json,
 * themes/, keybindings/, layouts/).  Cache and log are intentionally
 * NOT under here — they live under XDG_CACHE_HOME.
 * Returns a pointer to a static buffer; not thread-safe (same contract
 * as the previous in-file implementations). */
const char *netune_data_root(void);

/* Generic XDG helper: netune_xdg_dir("XDG_CACHE_HOME", "netune.log")
 * resolves <xdg-home>/netune/<sub>.  On Windows XDG_*_HOME maps to
 * APPDATA / LOCALAPPDATA.  sub may be NULL for the bare netune dir. */
const char *netune_xdg_dir(const char *env, const char *sub);

/* mkdir -p for the parent directory of filepath (both / and \ aware). */
void netune_ensure_dir(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* NETUNE_CONFIG_PATHS_H */
