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

/* Muted: dimmed secondary text color */
Element theme_muted(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.muted.has_color)
        return e | color(Color::RGB(t.muted.r, t.muted.g, t.muted.b));
    /* Fallback: fg + dim */
    if (t.fg.has_color)
        return e | color(Color::RGB(t.fg.r, t.fg.g, t.fg.b)) | dim;
    return e | dim;
}

/* Border: colored border for panels */
Element theme_border(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.border.has_color)
        return e | color(Color::RGB(t.border.r, t.border.g, t.border.b));
    return e;
}

/* Status colors */
Element theme_success(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.success.has_color)
        return e | color(Color::RGB(t.success.r, t.success.g, t.success.b));
    return e;
}

Element theme_warning(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.warning.has_color)
        return e | color(Color::RGB(t.warning.r, t.warning.g, t.warning.b));
    return e;
}

Element theme_error(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.error.has_color)
        return e | color(Color::RGB(t.error.r, t.error.g, t.error.b));
    return e;
}

/* Overlay background for popups */
Element theme_overlay_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.overlay_bg.has_color)
        return e | bgcolor(Color::RGB(t.overlay_bg.r, t.overlay_bg.g, t.overlay_bg.b));
    return theme_bg(e);
}

/* ── Convenience: direct ThemeColor access ──────────── */

const ThemeColor& theme_color_bg() {
    return ThemeManager::instance().current().bg;
}
const ThemeColor& theme_color_fg() {
    return ThemeManager::instance().current().fg;
}
const ThemeColor& theme_color_accent() {
    return ThemeManager::instance().current().accent;
}
const ThemeColor& theme_color_muted() {
    return ThemeManager::instance().current().muted;
}
const ThemeColor& theme_color_border() {
    return ThemeManager::instance().current().border;
}
