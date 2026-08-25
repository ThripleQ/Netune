#include "ui/theme.h"
#include "infra/log.h"
#include <yaml.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "compat/utf8.h"

/* ── Path separator helper ────────────────────────── */
#ifdef _WIN32
static const char PATH_SEP = '\\';
#else
static const char PATH_SEP = '/';
#endif

/* ── XDG / Windows config path helper ─────────────── */
static std::string xdg_config_path(const std::string &sub) {
#ifdef _WIN32
    /* Use getenv_utf8 so non-ASCII user names (e.g. Chinese) survive the
       ANSI→UTF-8 boundary. Plain getenv() returns the ANSI code page. */
    const char *d = getenv_utf8("APPDATA");
    if (d && d[0]) {
        return std::string(d) + "\\netune\\" + sub;
    }
    const char *home = getenv_utf8("USERPROFILE");
    if (!home) home = "C:\\";
    return std::string(home) + "\\AppData\\Roaming\\netune\\" + sub;
#else
    const char *d = getenv_utf8("XDG_CONFIG_HOME");
    if (d && d[0]) {
        return std::string(d) + "/netune/" + sub;
    }
    const char *home = getenv_utf8("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/netune/" + sub;
#endif
}

/* ── Hex ↔ ThemeColor ─────────────────────────────── */

ThemeColor theme_color_from_hex(const std::string &hex) {
    ThemeColor c;
    if (hex.empty() || hex[0] != '#') return c;  /* "none"/"transparent" → no color */

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
    if (!c.has_color) return "none";
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

    /* vip: gold-ish (defaults to warning) */
    if (!theme_.vip.has_color)
        theme_.vip = theme_.warning;

    /* svip: violet-ish (defaults to vip) */
    if (!theme_.svip.has_color)
        theme_.svip = theme_color_from_hex("#bb9af7");

    /* playlist: cyan-ish (defaults to accent) */
    if (!theme_.playlist.has_color)
        theme_.playlist = theme_.accent;

    /* logo: netease logo watermark (defaults to accent) */
    if (!theme_.logo.has_color && theme_.accent.has_color)
        theme_.logo = theme_.accent;

    /* NOTE: overlay_bg is intentionally NOT auto-derived. Its absence or
       explicit "none" means a transparent popup background (the terminal's
       own background shows through). See theme_overlay_bg(). */

    /* progress_track: lighter version of accent */
    if (!theme_.progress_track.has_color && theme_.accent.has_color)
        theme_.progress_track = lighten(theme_.accent, 0.65f);

    /* spectrum: defaults to accent if not set */
    if (!theme_.spectrum.has_color && theme_.accent.has_color)
        theme_.spectrum = theme_.accent;

    /* artist: differentiated secondary text; kept independent — no
       derivation, so it never silently follows another slot */
    if (!theme_.artist.has_color)
        theme_.artist = theme_.fg;

    /* popup_border: popup/action-sheet border, defaults to border */
    if (!theme_.popup_border.has_color && theme_.border.has_color)
        theme_.popup_border = theme_.border;
}

/* Resolve a theme name to a file path.
   All themes live under XDG_CONFIG_HOME/netune/data/themes/.
   No external scanning or fallback lookup is performed. */
std::string ThemeManager::resolve_path(const std::string &name) {
    if (name.empty() || name == "default") {
        return xdg_config_path(std::string("data/themes") + PATH_SEP + "default.yaml");
    }

    /* If it looks like a path (contains / or \), use directly */
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        return name;
    }

    /* Bare name → data/themes/<name>.yaml under the config dir */
    return xdg_config_path(std::string("data/themes") + PATH_SEP + name + ".yaml");
}

/* List available built-in theme names from data/themes/ under XDG config dir */
std::vector<std::string> ThemeManager::list_builtin_themes() {
    std::vector<std::string> names;
    /* Check the known bundled themes (avoids dirent dependency) */
    const char *known[] = {"default", "dracula", "catppuccin",
                           "netease_dark", "netease_light"};
    for (const char *n : known) {
        std::string p = xdg_config_path(std::string("data/themes") + PATH_SEP + n + ".yaml");
        if (access_utf8(p.c_str(), R_OK) == 0)
            names.push_back(n);
    }
    return names;
}

void ThemeManager::reset() {
    theme_ = Theme{};
}

bool ThemeManager::load(const std::string &yaml_path) {
    FILE *fp = fopen_utf8(yaml_path.c_str(), "rb");
    if (fp) {
        yaml_parser_t parser;
        yaml_event_t  event;
        yaml_parser_initialize(&parser);
        yaml_parser_set_input_file(&parser, fp);

        bool in_colors = false;
        std::string hex_bg, hex_fg, hex_accent;
        std::string hex_accent_bg, hex_border;
        std::string hex_success, hex_warning, hex_error, hex_overlay_bg, hex_spectrum;
        std::string hex_popup_border;
        std::string hex_vip, hex_svip, hex_playlist, hex_logo;
        std::string hex_progress_track;
        std::string hex_artist;
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
                        else if (current_field == "border")      hex_border      = val;
                        else if (current_field == "success")     hex_success     = val;
                        else if (current_field == "warning")     hex_warning     = val;
                        else if (current_field == "error")       hex_error       = val;
                        else if (current_field == "overlay_bg")  hex_overlay_bg  = val;
                        else if (current_field == "popup_border") hex_popup_border = val;
                        else if (current_field == "progress_track") hex_progress_track = val;
                        else if (current_field == "spectrum")    hex_spectrum    = val;
                        else if (current_field == "vip")         hex_vip         = val;
                        else if (current_field == "svip")        hex_svip        = val;
                        else if (current_field == "playlist")    hex_playlist    = val;
                        else if (current_field == "logo")         hex_logo        = val;
                        else if (current_field == "artist")       hex_artist      = val;
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
        if (!hex_border.empty())      theme_.border      = theme_color_from_hex(hex_border);
        if (!hex_success.empty())     theme_.success     = theme_color_from_hex(hex_success);
        if (!hex_warning.empty())     theme_.warning     = theme_color_from_hex(hex_warning);
        if (!hex_error.empty())       theme_.error       = theme_color_from_hex(hex_error);
        if (!hex_overlay_bg.empty())  theme_.overlay_bg  = theme_color_from_hex(hex_overlay_bg);
        if (!hex_popup_border.empty()) theme_.popup_border = theme_color_from_hex(hex_popup_border);
        if (!hex_progress_track.empty()) theme_.progress_track = theme_color_from_hex(hex_progress_track);
        if (!hex_spectrum.empty())    theme_.spectrum    = theme_color_from_hex(hex_spectrum);
        if (!hex_vip.empty())         theme_.vip         = theme_color_from_hex(hex_vip);
        if (!hex_svip.empty())        theme_.svip        = theme_color_from_hex(hex_svip);
        if (!hex_playlist.empty())    theme_.playlist    = theme_color_from_hex(hex_playlist);
        if (!hex_logo.empty())        theme_.logo        = theme_color_from_hex(hex_logo);
        if (!hex_artist.empty())      theme_.artist      = theme_color_from_hex(hex_artist);

        /* Derive any unset extended colors */
        derive_colors();

        /* Safety net: if fg or accent are still unset (empty/malformed YAML),
           fall back to hardcoded defaults. bg is intentionally excluded —
           leaving it unset lets the terminal's own background show through. */
        if (!theme_.fg.has_color || !theme_.accent.has_color) {
            LOG_WARN("Theme is missing fg or accent, applying safe defaults");
            theme_.fg    = theme_color_from_hex("#c0caf5");
            theme_.accent = theme_color_from_hex("#7aa2f7");
            derive_colors();
        }

        LOG_INFO("Theme loaded: '%s'  bg=%s fg=%s accent=%s accent_bg=%s border=%s progress_track=%s",
                 theme_.name.c_str(),
                 hex_bg.c_str(), hex_fg.c_str(), hex_accent.c_str(),
                 hex_accent_bg.c_str(), hex_border.c_str(),
                 hex_progress_track.c_str());
        return true;
    }

    /* File not found — fallback to safe hardcoded defaults.
       bg intentionally omitted to let terminal bg show through. */
    LOG_WARN("Cannot open theme: %s, using safe defaults", yaml_path.c_str());
    theme_.name = "Default Dark";
    theme_.fg = theme_color_from_hex("#c0caf5");
    theme_.accent = theme_color_from_hex("#7aa2f7");
    derive_colors();
    LOG_INFO("Using safe default theme");
    return false;
}
