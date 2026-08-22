/* netune_config.h — entry point of the built-in configuration UI.
 *
 * Formerly a separate `netune-config` executable; now built into the
 * main binary and launched as `netune --config` (see main.cpp).
 */
#ifndef NETUNE_CONFIG_H
#define NETUNE_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the interactive configuration TUI.  Takes over the terminal;
 * returns the process exit code when the user quits. */
int run_config(void);

#ifdef __cplusplus
}
#endif

#endif /* NETUNE_CONFIG_H */
