#include "ui/theme.h"
#include "infra/log.h"
#include <yaml.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <algorithm>

/* ── XDG path helper (local copy, avoids dependency on app.cpp) ── */
static std::string xdg_config_path(const std::string &sub) {
    const char *d = getenv("XDG_CONFIG_HOME");
    if (d && d[0]) {
        return std::string(d) + "/netune/" + sub;
    }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/netune/" + sub;
}

/* ── Hex ↔ ThemeColor ─────────────────────────────── */

ThemeColor theme_color_from_hex(const std::string &hex) {
    ThemeColor c;
    if (hex.empty() || hex[0] != '#') return c;

    const char *h = hex.c_str() + 1;
    size_t len = strlen(h);

    if (len == 3) {
        /* Short form #rgb → #rrggbb */
        unsigned long val = strtoul(h, nullptr, 16);
        c.r = (uint8_t)(((val >> 8) & 0xF) * 17);
        c.g = (uint8_t)(((val >> 4) & 0xF) * 17);
        c.b = (uint8_t)((val & 0xF) * 17);
        c.has_color = true;
    } else if (len == 6 || len == 8) {
        unsigned long val = strtoul(h, nullptr, 16);
        c.r = (uint8_t)((val >> 16) & 0xFF);
        c.g = (uint8_t)((val >> 8) & 0xFF);
        c.b = (uint8_t)(val & 0xFF);
        c.has_color = true;
        /* alpha (8-digit) is parsed but not stored — reserved for future */
    }
    return c;
}

std::string theme_color_to_hex(const ThemeColor &c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    return std::string(buf);
}

/* ── Color blending helpers ───────────────────────── */

static ThemeColor blend(const ThemeColor &a, const ThemeColor &b, float t) {
    ThemeColor r;
    r.r = (uint8_t)(a.r * (1.0f - t) + b.r * t);
    r.g = (uint8_t)(a.g * (1.0f - t) + b.g * t);
    r.b = (uint8_t)(a.b * (1.0f - t) + b.b * t);
    r.has_color = true;
    return r;
}

static ThemeColor darken(const ThemeColor &c, float t) {
    ThemeColor black = {0, 0, 0, true};
    return blend(c, black, t);
}

static ThemeColor lighten(const ThemeColor &c, float t) {
    ThemeColor white = {255, 255, 255, true};
    return blend(c, white, t);
}

/* ── ThemeManager ──────────────────────────────────── */

ThemeManager& ThemeManager::instance() {
    static ThemeManager mgr;
    return mgr;
}

/* Derive unset extended colors from the core palette.
   This ensures every color slot has a sensible value
   even if the theme YAML only specifies bg/fg/accent. */
void ThemeManager::derive_colors() {
    /* accent_bg: if not set, use accent itself */
    if (!theme_.accent_bg.has_color && theme_.accent.has_color)
        theme_.accent_bg = theme_.accent;

    /* muted: dimmed foreground (60% fg + 40% bg) */
    if (!theme_.muted.has_color && theme_.fg.has_color && theme_.bg.has_color)
        theme_.muted = blend(theme_.fg, theme_.bg, 0.45f);

    /* border: darkened background or mid-tone */
    if (!theme_.border.has_color && theme_.bg.has_color)
        theme_.border = lighten(theme_.bg, 0.12f);

    /* success: green-ish, derive from accent if unset */
    if (!theme_.success.has_color)
        theme_.success = theme_color_from_hex("#9ece6a");

    /* warning: yellow-ish */
    if (!theme_.warning.has_color)
        theme_.warning = theme_color_from_hex("#e0af68");

    /* error: red-ish */
    if (!theme_.error.has_color)
        theme_.error = theme_color_from_hex("#f7768e");

    /* overlay_bg: slightly lighter than bg */
    if (!theme_.overlay_bg.has_color && theme_.bg.has_color)
        theme_.overlay_bg = lighten(theme_.bg, 0.06f);
}

/* Resolve a theme name to a file path.
   Tries: data/themes/<name>.yaml, then XDG config dir. */
std::string ThemeManager::resolve_path(const std::string &name) {
    if (name.empty() || name == "default") {
        return xdg_config_path("themes/default.yaml");
    }

    /* If it looks like a path (contains /), use directly */
    if (name.find('/') != std::string::npos) {
        return name;
    }

    /* Bare name → try data/themes/<name>.yaml relative to CWD */
    std::string data_path = std::string("data/themes/") + name + ".yaml";
    if (access(data_path.c_str(), R_OK) == 0) {
        return data_path;
    }

    /* Then try XDG_CONFIG_HOME/themes/<name>.yaml */
    std::string xdg_path = xdg_config_path("themes/" + name + ".yaml");
    if (access(xdg_path.c_str(), R_OK) == 0) {
        return xdg_path;
    }

    /* Fallback: return data path even if it doesn't exist (load() will handle error) */
    return data_path;
}

/* List available built-in theme names from data/themes/ */
std::vector<std::string> ThemeManager::list_builtin_themes() {
    std::vector<std::string> names;
    /* Check a few known themes (avoids dirent dependency) */
    const char *known[] = {"default", "dracula", "catppuccin",
                           "netease_dark", "netease_light", "tokyonight",
                           "gruvbox", "nord", "rosepine"};
    for (const char *n : known) {
        std::string p = std::string("data/themes/") + n + ".yaml";
        if (access(p.c_str(), R_OK) == 0)
            names.push_back(n);
    }
    return names;
}

bool ThemeManager::load(const std::string &yaml_path) {
    FILE *fp = fopen(yaml_path.c_str(), "rb");
    if (fp) {
        yaml_parser_t parser;
        yaml_event_t  event;
        yaml_parser_initialize(&parser);
        yaml_parser_set_input_file(&parser, fp);

        bool in_colors = false;
        std::string hex_bg, hex_fg, hex_accent;
        std::string hex_accent_bg, hex_muted, hex_border;
        std::string hex_success, hex_warning, hex_error, hex_overlay_bg;
        std::string current_field;

        while (yaml_parser_parse(&parser, &event)) {
            if (event.type == YAML_STREAM_END_EVENT) { yaml_event_delete(&event); break; }
            if (event.type == YAML_SCALAR_EVENT) {
                const char *val = (const char*)event.data.scalar.value;
                if (!val) { yaml_event_delete(&event); continue; }

                if (strcmp(val, "colors") == 0) { in_colors = true; current_field.clear(); }
                else if (in_colors) {
                    if (current_field.empty()) {
                        current_field = val;
                    } else {
                        if      (current_field == "bg")          hex_bg          = val;
                        else if (current_field == "fg")          hex_fg          = val;
                        else if (current_field == "accent")      hex_accent      = val;
                        else if (current_field == "accent_bg")   hex_accent_bg   = val;
                        else if (current_field == "muted")       hex_muted       = val;
                        else if (current_field == "border")      hex_border      = val;
                        else if (current_field == "success")     hex_success     = val;
                        else if (current_field == "warning")     hex_warning     = val;
                        else if (current_field == "error")       hex_error       = val;
                        else if (current_field == "overlay_bg")  hex_overlay_bg  = val;
                        current_field.clear();
                    }
                } else if (strcmp(val, "name") == 0) {
                    yaml_event_delete(&event);
                    yaml_parser_parse(&parser, &event);
                    if (event.type == YAML_SCALAR_EVENT)
                        theme_.name = (const char*)event.data.scalar.value;
                }
            }
            if (event.type == YAML_MAPPING_END_EVENT) in_colors = false;
            yaml_event_delete(&event);
        }

        yaml_parser_delete(&parser);
        fclose(fp);

        /* Apply core colors */
        if (!hex_bg.empty())          theme_.bg          = theme_color_from_hex(hex_bg);
        if (!hex_fg.empty())          theme_.fg          = theme_color_from_hex(hex_fg);
        if (!hex_accent.empty())      theme_.accent      = theme_color_from_hex(hex_accent);
        /* Apply extended colors (if specified) */
        if (!hex_accent_bg.empty())   theme_.accent_bg   = theme_color_from_hex(hex_accent_bg);
        if (!hex_muted.empty())       theme_.muted       = theme_color_from_hex(hex_muted);
        if (!hex_border.empty())      theme_.border      = theme_color_from_hex(hex_border);
        if (!hex_success.empty())     theme_.success     = theme_color_from_hex(hex_success);
        if (!hex_warning.empty())     theme_.warning     = theme_color_from_hex(hex_warning);
        if (!hex_error.empty())       theme_.error       = theme_color_from_hex(hex_error);
        if (!hex_overlay_bg.empty())  theme_.overlay_bg  = theme_color_from_hex(hex_overlay_bg);

        /* Derive any unset extended colors */
        derive_colors();

        LOG_INFO("Theme loaded: '%s'  bg=%s fg=%s accent=%s accent_bg=%s muted=%s border=%s",
                 theme_.name.c_str(),
                 hex_bg.c_str(), hex_fg.c_str(), hex_accent.c_str(),
                 hex_accent_bg.c_str(), hex_muted.c_str(), hex_border.c_str());
        return true;
    }

    /* File not found — fallback to safe hardcoded defaults */
    LOG_WARN("Cannot open theme: %s, using safe defaults", yaml_path.c_str());
    theme_.name = "Default Dark";
    theme_.bg = theme_color_from_hex("#1a1b26");
    theme_.fg = theme_color_from_hex("#c0caf5");
    theme_.accent = theme_color_from_hex("#7aa2f7");
    derive_colors();
    LOG_INFO("Using safe default theme");
    return false;
}
