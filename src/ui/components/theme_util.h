#pragma once

#include <ftxui/dom/elements.hpp>
#include "ui/theme.h"

/* Wrap an element with the theme's foreground color */
ftxui::Element theme_fg(ftxui::Element e);

/* Wrap with theme's background color */
ftxui::Element theme_bg(ftxui::Element e);

/* Wrap with theme's accent color */
ftxui::Element theme_accent(ftxui::Element e);

/* Wrap with overlay background (or fallback to bg) */
ftxui::Element theme_overlay_bg(ftxui::Element e);
