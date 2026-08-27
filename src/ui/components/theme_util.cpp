#include "ui/components/theme_util.h"

#include "ui/color_util.h"

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

/* ── Helpers ────────────────────────────────────────── */
static Color pick_fg(const Theme &t) {
    return t.fg.has_color ? Color::RGB(t.fg.r, t.fg.g, t.fg.b) : kDefaultFg;
}
static Color pick_accent(const Theme &t) {
    return t.accent.has_color ? Color::RGB(t.accent.r, t.accent.g, t.accent.b) : kDefaultAccent;
}
static Color pick_progress_track(const Theme &t) {
    /* The unfilled track is the FILLED part's (accent) colour lightened,
       so it always reads as a lighter version of the progress — no need
       for a fixed, independent track colour that can go out of sync. */
    const ThemeColor &a = t.accent.has_color ? t.accent : ThemeColor{122, 162, 247, true};
    return Color::RGB(
        (int)(a.r + (255 - a.r) * 0.65f),
        (int)(a.g + (255 - a.g) * 0.65f),
        (int)(a.b + (255 - a.b) * 0.65f));
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

static Color pick_artist(const Theme &t) {
    if (t.artist.has_color) return Color::RGB(t.artist.r, t.artist.g, t.artist.b);
    return kDefaultMuted;
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
    const ThemeColor &c = t.overlay_bg;
    if (c.has_color)
        return e | bgcolor(Color::RGB(c.r, c.g, c.b));
    /* overlay_bg none/unset: use the terminal's own background — clear the
       area underneath (spaces, no color codes) so stale content does not
       leak through, while the background stays the terminal's own. */
    return clear_under(e);
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

Element theme_artist(Element e) {
    return e | color(pick_artist(ThemeManager::instance().current()));
}

Element theme_warning(Element e) {
    auto &t = ThemeManager::instance().current();
    return e | color(t.warning.has_color ? Color::RGB(t.warning.r, t.warning.g, t.warning.b)
                                        : kDefaultWarning);
}

/* Success: green-ish (downloadable/entitled tiers). */
Element theme_success(Element e) {
    auto &t = ThemeManager::instance().current();
    return e | color(t.success.has_color ? Color::RGB(t.success.r, t.success.g, t.success.b)
                                        : kDefaultSuccess);
}

Element theme_error(Element e) {
    auto &t = ThemeManager::instance().current();
    return e | color(t.error.has_color ? Color::RGB(t.error.r, t.error.g, t.error.b)
                                      : kDefaultError);
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

/* Popup / action-sheet border color (separate from the main border slot) */
Element theme_popup_border(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor c = t.popup_border.has_color ? t.popup_border : t.border;
    if (c.has_color)
        return e | color(Color::RGB(c.r, c.g, c.b));
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

/* Modal-open selection marker: a compact "> " colour block keeping the
   row's dedicated colour (playlist / VIP) when the theme defines one. */
Element theme_sel_marker(bool is_playlist, bool is_vip) {
    const auto &th = ThemeManager::instance().current();
    Element mark = text("> ") | bold;
    if (is_playlist) {
        mark = theme_playlist_sel_bg(mark);
        mark = mark | color(theme_selection_text(
            th.playlist.has_color ? th.playlist : th.accent));
    } else if (is_vip) {
        mark = theme_vip_sel_bg(mark);
        mark = mark | color(theme_selection_text(
            th.vip.has_color ? th.vip : th.warning));
    } else {
        mark = theme_selection(mark);
    }
    return mark;
}

/* Background warning: whole-row bg tinted with a darkened warning color so
   text stays readable (mirrors theme_vip_bg's dark-theme treatment). */
Element theme_warning_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor c = t.warning.has_color ? t.warning : ThemeColor{224, 175, 104, true};
    c = scale_tc(c, 0.45f, theme_bg_is_light(t));
    return e | bgcolor(Color::RGB(c.r, c.g, c.b));
}

/* ── Extended semantic colors ──────────────────────── */

/* Selection: sets both bgcolor (accent_bg) and text colour. The text
   colour is the WCAG-readable version of the fg over that background
   (hue-preserving contrast via netune::color_readable_on_bg), so every
   theme_selection() call site — top_bar, group_list, action_sheet,
   song_list filter rows — gets the SAME readable-and-pleasant highlight
   as the song-list selection instead of a flat fg that can wash out. */
Element theme_selection(Element e) {
    auto &t = ThemeManager::instance().current();
    ThemeColor sel_bg = t.accent_bg.has_color
        ? t.accent_bg
        : (t.accent.has_color ? t.accent : ThemeColor{80, 80, 80, true});
    ThemeColor fg = t.fg.has_color ? t.fg : ThemeColor{255, 255, 255, true};
    ThemeColor tc = netune::color_readable_on_bg(fg, sel_bg);
    e = e | bgcolor(Color::RGB(sel_bg.r, sel_bg.g, sel_bg.b));
    e = e | color(Color::RGB(tc.r, tc.g, tc.b));
    return e;
}

/* ── Selection text colour ───────────────────────────
   Both public functions delegate to netune::color_readable_on_bg (the
   WCAG hue-preserving algorithm in color_util.cpp), so the whole app
   shares ONE colour strategy — selected text everywhere keeps its
   category hue and is brightness-adjusted for >= 4.5:1 contrast, with
   saturation fallback to avoid pure black/white. */

/* Selection text colour for a selected row: the WCAG-readable version of
   the row's own background colour (same hue, brightness-adjusted). Used
   when the whole row collapses to a single inverse colour. */
Color theme_selection_text(const ThemeColor &bg) {
    if (!bg.has_color) {
        const auto &t = ThemeManager::instance().current();
        return t.fg.has_color ? Color::RGB(t.fg.r, t.fg.g, t.fg.b)
                              : Color::RGB(255, 255, 255);
    }
    ThemeColor out = netune::color_readable_on_bg(bg, bg);
    return Color::RGB(out.r, out.g, out.b);
}

/* Per-category selected text colour: keeps the TEXT's own hue (title =
   fg, artist = custom colour, badge = playlist/VIP) and brightness-shifts
   it against the row background for >= 4.5:1 contrast. */
Color theme_text_on_bg(const ThemeColor &text, const ThemeColor &bg) {
    if (!text.has_color || !bg.has_color) return theme_selection_text(bg);
    ThemeColor out = netune::color_readable_on_bg(text, bg);
    return Color::RGB(out.r, out.g, out.b);
}
