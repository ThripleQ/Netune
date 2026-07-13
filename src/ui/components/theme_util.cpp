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

Element theme_overlay_bg(Element e) {
    auto &t = ThemeManager::instance().current();
    if (t.overlay_bg.has_color)
        return e | bgcolor(Color::RGB(t.overlay_bg.r, t.overlay_bg.g, t.overlay_bg.b));
    /* fallback: bg tinted slightly */
    if (t.bg.has_color) {
        uint8_t r = t.bg.r > 20 ? t.bg.r - 10 : 0;
        uint8_t g = t.bg.g > 20 ? t.bg.g - 10 : 0;
        uint8_t b = t.bg.b > 20 ? t.bg.b - 10 : 0;
        return e | bgcolor(Color::RGB(r, g, b));
    }
    return e;
}
