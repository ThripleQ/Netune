#pragma once

#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ui/theme.h"

using namespace ftxui;

/* Core theme colors (legacy) */
Element theme_fg(Element e);
Element theme_bg(Element e);
Element theme_accent(Element e);

/* Overlay/popup background (overlay_bg, falls back to bg, then transparent) */
Element theme_overlay_bg(Element e);

/* ── Extended semantic colors ──────────────────────── */
/* Selection: sets bgcolor (accent_bg) and text color (fg) */
Element theme_selection(Element e);

/* Border / divider lines color (applied outermost) */
Element theme_border(Element e);

/* Progress bar unfilled track background */
Element theme_progress_track(Element e);

/* Spectrum bar color */
Element theme_spectrum(Element e);

/* VIP (◆) marker color */
Element theme_vip(Element e);

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
