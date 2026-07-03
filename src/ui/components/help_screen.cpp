#include "ui/components/help_screen.h"
#include "ui/components/theme_util.h"
#include <cstdio>
using namespace ftxui;

Element render_help_screen(const AppState &s) {
    (void)s;

    /* Build keybinding help entries */
    auto entry = [](const char *key, const char *desc) {
        return hbox(Elements{
            text("  "),
            text(key) | bold,
            text("  —  "),
            text(desc),
        });
    };

    Elements col;
    col.push_back(text(" Netune v2.0.0 — Help ") | bold | center | underlined);
    col.push_back(separator());
    col.push_back(entry("Tab",     "Switch panel (groups / songs)"));
    col.push_back(entry("j / Down","Move down"));
    col.push_back(entry("k / Up",  "Move up"));
    col.push_back(entry("Enter",   "Play selected song"));
    col.push_back(entry("Space",   "Play / Pause"));
    col.push_back(entry("n",       "Next track"));
    col.push_back(entry("p",       "Previous track"));
    col.push_back(entry("Right",   "Seek forward"));
    col.push_back(entry("Left",    "Seek backward"));
    col.push_back(entry("+ / =",   "Volume up"));
    col.push_back(entry("-",       "Volume down"));
    col.push_back(entry("m",       "Toggle mute"));
    col.push_back(entry("s",       "Stop playback"));
    col.push_back(entry("l",       "Cycle loop mode"));
    col.push_back(entry("/",       "Search"));
    col.push_back(entry("q / Esc", "Quit"));
    col.push_back(entry("?",       "Toggle this help"));
    col.push_back(separator());
    col.push_back(text(" Press ? again or Escape to close ") | dim | center);

    auto help_box = vbox(std::move(col));
    auto framed = help_box | border | center | clear_under | bgcolor(Color::RGB(20,20,30));

    return framed;
}
