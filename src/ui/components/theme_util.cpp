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

static Color pick_vip(const Theme &t) {
    return t.vip.has_color ? Color::RGB(t.vip.r, t.vip.g, t.vip.b)
        : (t.warning.has_color ? Color::RGB(t.warning.r, t.warning.g, t.warning.b)
                               : kDefaultFg);
}

static Color pick_playlist(const Theme &t) {
    return t.playlist.has_color ? Color::RGB(t.playlist.r, t.playlist.g, t.playlist.b)
        : (t.accent.has_color ? Color::RGB(t.accent.r, t.accent.g, t.accent.b)
                              : kDefaultAccent);
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

Element theme_overlay_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor c = t.overlay_bg.has_color ? t.overlay_bg
                 : (t.bg.has_color ? t.bg : ThemeColor{});
    if (c.has_color)
        return e | bgcolor(Color::RGB(c.r, c.g, c.b));
    return e;  /* both transparent → terminal background shows through */
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

Element theme_vip(Element e) {
    return e | color(pick_vip(ThemeManager::instance().current()));
}

Element theme_playlist(Element e) {
    return e | color(pick_playlist(ThemeManager::instance().current()));
}

Element theme_logo(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor c = t.logo.has_color ? t.logo : t.accent;
    return e | color(Color::RGB(c.r, c.g, c.b));
}

Element theme_border(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.border.has_color)
        return e | color(Color::RGB(t.border.r, t.border.g, t.border.b));
    return e;
}

/* ── Background markers (whole-row) ───────────────────
   VIP/playlist rows tint the row background with the marker color.
   Dark themes: darken the marker so bright text stays readable.
   Light themes: lighten it (text is dark there). */
static ThemeColor scale_tc(const ThemeColor &c, float t, bool toward_white) {
    ThemeColor out = c;
    if (toward_white) {
        out.r = (uint8_t)(c.r + (255 - c.r) * t);
        out.g = (uint8_t)(c.g + (255 - c.g) * t);
        out.b = (uint8_t)(c.b + (255 - c.b) * t);
    } else {
        out.r = (uint8_t)(c.r * (1.0f - t));
        out.g = (uint8_t)(c.g * (1.0f - t));
        out.b = (uint8_t)(c.b * (1.0f - t));
    }
    out.has_color = true;
    return out;
}

static bool theme_bg_is_light(const Theme &t) {
    if (!t.bg.has_color) return false;
    return (t.bg.r + t.bg.g + t.bg.b) / 3 >= 128;
}

Element theme_vip_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor c = t.vip.has_color ? t.vip : t.warning;
    c = scale_tc(c, 0.45f, theme_bg_is_light(t));
    return e | bgcolor(Color::RGB(c.r, c.g, c.b));
}

Element theme_playlist_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor c = t.playlist.has_color ? t.playlist : t.accent;
    c = scale_tc(c, 0.45f, theme_bg_is_light(t));
    return e | bgcolor(Color::RGB(c.r, c.g, c.b));
}

/* Selected-row variants: the FULL marker color (accentuated) so the
   selection reads as an intensified marker. */
Element theme_vip_sel_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor c = t.vip.has_color ? t.vip : t.warning;
    return e | bgcolor(Color::RGB(c.r, c.g, c.b));
}

Element theme_playlist_sel_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor c = t.playlist.has_color ? t.playlist : t.accent;
    return e | bgcolor(Color::RGB(c.r, c.g, c.b));
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
