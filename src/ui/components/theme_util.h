#pragma once

#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ui/theme.h"

using namespace ftxui;

/* ── Core theme colors (legacy) ────────────────────── */
Element theme_fg(Element e);
Element theme_bg(Element e);
Element theme_accent(Element e);

/* ── Extended semantic colors ──────────────────────── */
/* Selection background (used with inverted or bgcolor) */
Element theme_selection(Element e);

/* Muted/dimmed text (secondary info) */
Element theme_muted(Element e);

/* Border / divider color */
Element theme_border(Element e);

/* Status colors */
Element theme_success(Element e);
Element theme_warning(Element e);
Element theme_error(Element e);

/* Overlay/popup background */
Element theme_overlay_bg(Element e);

/* ── Convenience: get ThemeColor directly ──────────── */
const ThemeColor& theme_color_bg();
const ThemeColor& theme_color_fg();
const ThemeColor& theme_color_accent();
const ThemeColor& theme_color_muted();
const ThemeColor& theme_color_border();
