#include "ui/components/theme_util.h"

/* ── Hardcoded safe defaults (Tokyo Night) ───────────── */
/* Used when a theme color slot has never been set.
   Exception: bg has NO fallback — when unset, the terminal's own
   background shows through (no bgcolor decoration applied). */
static const Color kDefaultFg     = Color::RGB(192, 202, 245);
static const Color kDefaultAccent = Color::RGB(122, 162, 247);
static const Color kDefaultMuted  = Color::RGB( 86,  95, 137);
static const Color kDefaultBorder = Color::RGB( 41,  46,  66);
static const Color kDefaultSpectrum = kDefaultAccent;
static const Color kDefaultSuccess= Color::RGB(158, 206, 106);
static const Color kDefaultWarning= Color::RGB(224, 175, 104);
static const Color kDefaultError  = Color::RGB(247, 118, 142);
/* lighten(kDefaultAccent, 0.65f) pre-computed: RGB(122*0.35+255*0.65, 162*0.35+255*0.65, 247*0.35+255*0.65) */
static const Color kDefaultProgressTrack = Color::RGB(208, 222, 252);

/* ── Helpers ────────────────────────────────────────── */
static Color pick_fg(const Theme &t) {
    return t.fg.has_color ? Color::RGB(t.fg.r, t.fg.g, t.fg.b) : kDefaultFg;
}
static Color pick_accent(const Theme &t) {
    return t.accent.has_color ? Color::RGB(t.accent.r, t.accent.g, t.accent.b) : kDefaultAccent;
}
static Color pick_progress_track(const Theme &t) {
    return t.progress_track.has_color
        ? Color::RGB(t.progress_track.r, t.progress_track.g, t.progress_track.b)
        : kDefaultProgressTrack;
}

static Color pick_spectrum(const Theme &t) {
    return t.spectrum.has_color ? Color::RGB(t.spectrum.r, t.spectrum.g, t.spectrum.b) : kDefaultSpectrum;
}

/* ── Core theme colors ─────────────────────────────── */

Element theme_fg(Element e) {
    return e | color(pick_fg(ThemeManager::instance().current()));
}

Element theme_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.bg.has_color)
        return e | bgcolor(Color::RGB(t.bg.r, t.bg.g, t.bg.b));
    return e;  /* no bg → terminal native background shows through */
}

Element theme_accent(Element e) {
    return e | color(pick_accent(ThemeManager::instance().current()));
}

Element theme_spectrum(Element e) {
    return e | color(pick_spectrum(ThemeManager::instance().current()));
}

Element theme_progress_track(Element e) {
    return e | bgcolor(pick_progress_track(ThemeManager::instance().current()));
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
