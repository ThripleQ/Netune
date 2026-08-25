#ifndef NETUNE_DEFAULTS_H
#define NETUNE_DEFAULTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Ensure the full default data tree exists at the canonical data root:
 * creates any missing directories AND default file contents (config.json,
 * layouts/, keybindings/, themes/). Existing files are NEVER overwritten.
 * Called once at startup. */
void netune_ensure_default_data_tree(void);

#ifdef __cplusplus
}
#endif

#endif /* NETUNE_DEFAULTS_H */
