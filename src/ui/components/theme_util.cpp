#include "ui/components/theme_util.h"

/* ── Core theme colors (legacy) ────────────────────── */

Element theme_fg(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.fg.has_color)
        return e | color(Color::RGB(t.fg.r, t.fg.g, t.fg.b));
    return e;
}

Element theme_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.bg.has_color)
        return e | bgcolor(Color::RGB(t.bg.r, t.bg.g, t.bg.b));
    return e;
}

Element theme_accent(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.accent.has_color)
        return e | color(Color::RGB(t.accent.r, t.accent.g, t.accent.b));
    return e;
}

/* ── Extended semantic colors ──────────────────────── */

/* Selection: sets both bgcolor (accent_bg) and color (fg) for
   proper contrast on selected items */
Element theme_selection(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.accent_bg.has_color) {
        e = e | bgcolor(Color::RGB(t.accent_bg.r, t.accent_bg.g, t.accent_bg.b));
        /* Use fg or white for text on selection bg */
        if (t.fg.has_color)
            e = e | color(Color::RGB(t.fg.r, t.fg.g, t.fg.b));
    }
    return e;
}
