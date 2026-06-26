#include "ui/components/theme_util.h"
#include <ftxui/dom/elements.hpp>
using namespace ftxui;

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
