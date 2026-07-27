#include "ui/components/theme_util.h"

/* ── Hardcoded safe defaults (Tokyo Night) ───────────── */
/* Used when a theme color slot has never been set. */
static const Color kDefaultBg     = Color::RGB( 26,  27,  38);
static const Color kDefaultFg     = Color::RGB(192, 202, 245);
static const Color kDefaultAccent = Color::RGB(122, 162, 247);
static const Color kDefaultMuted  = Color::RGB( 86,  95, 137);
static const Color kDefaultBorder = Color::RGB( 41,  46,  66);
static const Color kDefaultSuccess= Color::RGB(158, 206, 106);
static const Color kDefaultWarning= Color::RGB(224, 175, 104);
static const Color kDefaultError  = Color::RGB(247, 118, 142);

/* ── Helpers ────────────────────────────────────────── */
static Color pick_fg(const Theme &t) {
    return t.fg.has_color ? Color::RGB(t.fg.r, t.fg.g, t.fg.b) : kDefaultFg;
}
static Color pick_bg(const Theme &t) {
    return t.bg.has_color ? Color::RGB(t.bg.r, t.bg.g, t.bg.b) : kDefaultBg;
}
static Color pick_accent(const Theme &t) {
    return t.accent.has_color ? Color::RGB(t.accent.r, t.accent.g, t.accent.b) : kDefaultAccent;
}

/* ── Core theme colors ─────────────────────────────── */

Element theme_fg(Element e) {
    return e | color(pick_fg(ThemeManager::instance().current()));
}

Element theme_bg(Element e) {
    return e | bgcolor(pick_bg(ThemeManager::instance().current()));
}

Element theme_accent(Element e) {
    return e | color(pick_accent(ThemeManager::instance().current()));
}

/* ── Extended semantic colors ──────────────────────── */

/* Selection: sets both bgcolor (accent_bg) and color (fg) */
Element theme_selection(Element e) {
    auto &t = ThemeManager::instance().current();
    Color sel_bg = t.accent_bg.has_color
        ? Color::RGB(t.accent_bg.r, t.accent_bg.g, t.accent_bg.b)
        : pick_accent(t);
    e = e | bgcolor(sel_bg);
    e = e | color(pick_fg(t));
    return e;
}
