/* defaults.cpp — bundled default resources (config.json, layouts/,
 * keybindings/, themes/) written to the canonical data root on first run.
 *
 * This module exists so the default templates and the "create whatever is
 * missing, never overwrite existing" logic live outside app.cpp.  It is
 * pure file I/O — no UI, no state beyond the static strings below.
 */
#include "infra/defaults.h"
#include "infra/config_paths.h"
#include "infra/log.h"
#include "compat/utf8.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

static const char *DEFAULT_CONFIG_JSON =
    "{\n"
    "  \"version\": \"1.0\",\n"
    "  \"audio\": { \"backend\": \"auto\", \"volume\": 80 },\n"
    "  \"playback\": { \"loop_mode\": 0, \"seek_step_sec\": 5 },\n"
    "  \"ui\": { \"theme\": \"default\", \"layout\": \"default\", \"keybindings\": \"default\" },\n"
    "  \"music_sources\": {\n"
    "    \"local\": { \"enabled\": true, \"dirs\": [] },\n"
    "    \"netease\": { \"enabled\": true }\n"
    "  }\n"
    "}\n";

static const char *DEFAULT_LAYOUT_YAML =
    "layout:\n"
    "  type: \"vertical\"\n"
    "  children:\n"
    "    - component: \"top_bar\"\n"
    "      height: 1\n"
    "    - type: \"horizontal\"\n"
    "      flex: 1\n"
    "      children:\n"
    "        - component: \"group_list\"\n"
    "          width: 20\n"
    "        - component: \"song_list\"\n"
    "          flex: 1\n"
    "    - component: \"status_bar\"\n"
    "      height: 2\n";

static const char *DEFAULT_KEYBINDINGS_YAML =
    "keybindings:\n"
    "  move_down:     [\"j\", \"down\"]\n"
    "  move_up:       [\"k\", \"up\"]\n"
    "  panel_switch:  [\"tab\"]\n"
    "  play_pause:    [\"space\"]\n"
    "  play_select:   [\"enter\"]\n"
    "  next_track:    [\"n\"]\n"
    "  prev_track:    [\"p\"]\n"
    "  seek_forward:   [\"right\"]\n"
    "  seek_backward:  [\"left\"]\n"
    "  volume_up:     [\"+\", \"=\"]\n"
    "  volume_down:   [\"-\"]\n"
    "  open_search:   [\"ctrl+/\"]\n"
    "  stop:          [\"s\"]\n"
    "  toggle_mute:   [\"m\"]\n"
    "  cycle_loop:    [\"r\"]\n"
    "  toggle_lyrics: [\"l\"]\n"
    "  show_help:     [\"?\"]\n"
    "  show_actions:  [\"ctrl+x\"]\n"
    "  quit:          [\"q\"]\n";

static const char *DEFAULT_THEME_DEFAULT_YAML =
    "name: \"Tokyo Night\"\n"
    "colors:\n"
    "  bg: \"#1a1b26\"\n"
    "  fg: \"#c0caf5\"\n"
    "  accent: \"#7aa2f7\"\n"
    "  accent_bg: \"#33467c\"\n"
    "  border: \"#292e42\"\n"
    "  border_style: \"sharp\"\n"
    "  success: \"#9ece6a\"\n"
    "  warning: \"#e0af68\"\n"
    "  error: \"#f7768e\"\n"
    "  vip: \"#e0af68\"\n"
    "  svip: \"#bb9af7\"\n"
    "  playlist: \"#7dcfff\"\n"
    "  logo: \"#7dcfff\"\n"
    "  overlay_bg: \"#16161e\"\n";

static const char *DEFAULT_THEME_CATPPUCCIN_YAML =
    "name: \"Catppuccin Mocha\"\n"
    "colors:\n"
    "  bg: \"#1e1e2e\"\n"
    "  fg: \"#cdd6f4\"\n"
    "  accent: \"#89b4fa\"\n"
    "  vip: \"#f9e2af\"\n"
    "  svip: \"#b4befe\"\n"
    "  playlist: \"#94e2d5\"\n"
    "  logo: \"#94e2d5\"\n";

static const char *DEFAULT_THEME_DRACULA_YAML =
    "name: \"Dracula\"\n"
    "colors:\n"
    "  bg: \"#282a36\"\n"
    "  fg: \"#f8f8f2\"\n"
    "  accent: \"#bd93f9\"\n"
    "  vip: \"#f1fa8c\"\n"
    "  svip: \"#bd93f9\"\n"
    "  playlist: \"#8be9fd\"\n"
    "  logo: \"#8be9fd\"\n";

static const char *DEFAULT_THEME_NETEASE_DARK_YAML =
    "name: \"Netease Dark\"\n"
    "colors:\n"
    "  bg: \"#1a1a2e\"\n"
    "  fg: \"#c8c8dc\"\n"
    "  accent: \"#e3322d\"\n"
    "  vip: \"#e8c547\"\n"
    "  svip: \"#b78af7\"\n"
    "  playlist: \"#4aa3df\"\n"
    "  logo: \"#4aa3df\"\n";

static const char *DEFAULT_THEME_NETEASE_LIGHT_YAML =
    "name: \"Netease Light\"\n"
    "colors:\n"
    "  bg: \"#f5f5f5\"\n"
    "  fg: \"#333333\"\n"
    "  accent: \"#d43c33\"\n"
    "  vip: \"#c9a227\"\n"
    "  svip: \"#8a5cf6\"\n"
    "  playlist: \"#2d7bb5\"\n"
    "  logo: \"#2d7bb5\"\n";

void netune_ensure_default_data_tree(void) {
    const char *root = netune_data_root();

    /* Ensure the root directory exists */
    netune_ensure_dir(root);

    /* Helper: create a default file (with parent dirs) if it
       does not already exist. Never touches existing non-empty files.
       0-byte files count as missing and get rebuilt. */
    auto ensure_file = [&](const char *rel, const char *content) {
        char path[2048];
        snprintf(path, sizeof(path), "%s" PATH_SEP "%s", root, rel);
        FILE *f = fopen_utf8(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            if (sz > 0) return;  /* non-empty: already there */
            LOG_WARN("Rebuilding empty data file: %s", path);
        }
        netune_ensure_dir(path);
        f = fopen_utf8(path, "w");
        if (f) {
            fputs(content, f);
            fclose(f);
            LOG_INFO("Created default data file: %s", path);
        } else {
            LOG_WARN("Failed to create default data file: %s", path);
        }
    };

    /* config.json at the data root */
    ensure_file("config.json", DEFAULT_CONFIG_JSON);
    /* layouts */
    ensure_file("layouts/default.yaml", DEFAULT_LAYOUT_YAML);
    /* keybindings */
    ensure_file("keybindings/default.yaml", DEFAULT_KEYBINDINGS_YAML);
    /* themes — all bundled defaults */
    ensure_file("themes/default.yaml",       DEFAULT_THEME_DEFAULT_YAML);
    ensure_file("themes/catppuccin.yaml",    DEFAULT_THEME_CATPPUCCIN_YAML);
    ensure_file("themes/dracula.yaml",       DEFAULT_THEME_DRACULA_YAML);
    ensure_file("themes/netease_dark.yaml",  DEFAULT_THEME_NETEASE_DARK_YAML);
    ensure_file("themes/netease_light.yaml", DEFAULT_THEME_NETEASE_LIGHT_YAML);
}
