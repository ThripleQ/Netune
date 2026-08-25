#pragma once

#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ui/theme.h"

using namespace ftxui;

/* Core theme colors (legacy) */
Element theme_fg(Element e);
Element theme_bg(Element e);
Element theme_accent(Element e);

/* Overlay/popup background (overlay_bg; terminal's own background when
   unset/none — area is cleared underneath so stale content does not leak) */
Element theme_overlay_bg(Element e);

/* ── Extended semantic colors ──────────────────────── */
/* Selection: sets bgcolor (accent_bg) and text color (fg) */
Element theme_selection(Element e);

/* Inverted text color for a selected row: the complement of the given
   background color (255 - each channel), so the text stays readable on
   the selection highlight regardless of theme. */
Color theme_selection_text(const ThemeColor &bg);

/* Border / divider lines color (applied outermost) */
Element theme_border(Element e);

/* Popup / action-sheet border color (separate slot) */
Element theme_popup_border(Element e);

/* Progress bar unfilled track background */
Element theme_progress_track(Element e);

/* Spectrum bar color */
Element theme_spectrum(Element e);

/* VIP (◆) marker color */
Element theme_vip(Element e);

/* Warning color — marks tiers with no entitlement when they're shown
   but not playable/downloadable at the current account's level. */
Element theme_warning(Element e);

/* Success color — marks tiers that are downloadable / entitled. */
Element theme_success(Element e);

/* Error color — errors / failed operations */
Element theme_error(Element e);

/* Background warning — whole-row bg for no-entitlement tiers */
Element theme_warning_bg(Element e);

/* Artist name — differentiated secondary text color */
Element theme_artist(Element e);

/* Playlist (▣) marker color */
Element theme_playlist(Element e);

/* Netease logo watermark color */
Element theme_logo(Element e);

/* Whole-row background markers (darkened marker color) */
Element theme_vip_bg(Element e);
Element theme_playlist_bg(Element e);

/* Selected-row marker backgrounds (full marker color) */
Element theme_vip_sel_bg(Element e);
Element theme_playlist_sel_bg(Element e);

/* Modal-open selection marker: a compact "> " colour block that keeps the
   row's dedicated colour (playlist / VIP) when the theme defines one,
   mirroring the full-row selection logic. */
Element theme_sel_marker(bool is_playlist, bool is_vip);
