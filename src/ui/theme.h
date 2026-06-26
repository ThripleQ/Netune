#pragma once

#include <string>
#include <cstdint>

/* ── Theme colors ──────────────────────────────────── */
struct ThemeColor {
    uint8_t r = 255, g = 255, b = 255;
    bool    has_color = false;
};

/* ── Theme ──────────────────────────────────────────── */
struct Theme {
    std::string name = "default";
    ThemeColor bg;       /* background */
    ThemeColor fg;       /* foreground */
    ThemeColor accent;   /* accent (selection, highlights) */
};

/* ── Theme manager (singleton) ──────────────────────── */
class ThemeManager {
public:
    static ThemeManager& instance();
    bool load(const std::string &yaml_path);
    const Theme& current() const { return theme_; }

private:
    ThemeManager() = default;
    Theme theme_;
};

/* ── Color helper: hex "#1e1e2e" → ThemeColor ─────── */
ThemeColor theme_color_from_hex(const std::string &hex);
