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
/* Selection: sets bgcolor (accent_bg) and text color (fg) */
Element theme_selection(Element e);
