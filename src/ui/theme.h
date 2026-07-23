#pragma once

#include <string>
#include <cstdint>
#include <vector>

/* ── Theme colors ──────────────────────────────────── */
struct ThemeColor {
    uint8_t r = 255, g = 255, b = 255;
    bool    has_color = false;
};

/* ── Theme ──────────────────────────────────────────── */
/* Extended theme model with semantic color slots.
   Legacy bg/fg/accent are kept for backward compat;
   new slots default to deriving from accent/bg if unset. */
struct Theme {
    std::string name = "default";

    /* Core colors (legacy, always present) */
    ThemeColor bg;            /* background          */
    ThemeColor fg;            /* foreground / text   */
    ThemeColor accent;        /* accent (primary)    */

    /* Extended semantic colors (optional, derived if unset) */
    ThemeColor accent_bg;     /* selection background       */
    ThemeColor muted;         /* dimmed/secondary text      */
    ThemeColor border;        /* border / divider lines     */
    ThemeColor success;       /* success / online indicator */
    ThemeColor warning;       /* warning / VIP badge        */
    ThemeColor error;         /* error / important          */
    ThemeColor overlay_bg;    /* overlay/popup background   */
};

/* ── Theme manager (singleton) ──────────────────────── */
class ThemeManager {
public:
    static ThemeManager& instance();
    bool load(const std::string &yaml_path);
    const Theme& current() const { return theme_; }

    /* Resolve a theme name to a file path.
       - "default" or NULL → XDG_CONFIG_HOME/netune/data/themes/default.yaml
       - bare name (e.g. "dracula") → XDG_CONFIG_HOME/netune/data/themes/<name>.yaml
       - path with '/' → used as-is
       Default files are auto-created on startup by ensure_default_data_tree()
       in app.cpp; no other locations are scanned. */
    static std::string resolve_path(const std::string &name);

    /* List available built-in theme names */
    static std::vector<std::string> list_builtin_themes();

private:
    ThemeManager() = default;
    Theme theme_;

    /* Derive unset extended colors from core colors */
    void derive_colors();
};

/* ── Color helper: hex "#1e1e2e" → ThemeColor ─────── */
ThemeColor theme_color_from_hex(const std::string &hex);

/* ── Color helper: ThemeColor → hex string ────────── */
std::string theme_color_to_hex(const ThemeColor &c);
