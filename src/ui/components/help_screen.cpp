#include "ui/components/help_screen.h"
#include "ui/components/theme_util.h"
#include "ui/theme.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace ftxui;

Element render_help_screen(const AppState &s) {
    (void)s;

    /* Key column width — all keys are ASCII so manual padding is safe */
    constexpr int KEYW = 12;

    auto entry = [](const char *key, const char *desc) {
        size_t klen = strlen(key);
        size_t pad = klen < KEYW ? KEYW - klen : 1;
        return hbox(Elements{
            text("  "),
            text(std::string(key) + std::string(pad, ' ')) | bold,
            text(desc),
        });
    };

    auto group = [](const char *title, Elements items) {
        Elements col;
        col.push_back(text(title) | bold | underlined);
        col.push_back(text(""));
        for (auto &e : items) col.push_back(e);
        return vbox(std::move(col));
    };

    /* ── Navigation ── */
    auto nav = group(" Navigation ", Elements{
        entry("Tab",       "Switch panel (groups / songs)"),
        entry("j / Down",  "Move down"),
        entry("k / Up",    "Move up"),
        entry("Enter",     "Play selected song"),
        entry("/",         "Search"),
    });

    /* ── Playback ── */
    auto play = group(" Playback ", Elements{
        entry("Space",    "Play / Pause"),
        entry("n",        "Next track"),
        entry("p",        "Previous track"),
        entry("Right",    "Seek forward"),
        entry("Left",     "Seek backward"),
        entry("s",        "Stop playback"),
    });

    /* ── Volume ── */
    auto vol = group(" Volume ", Elements{
        entry("+ / =",   "Volume up"),
        entry("-",       "Volume down"),
        entry("m",       "Toggle mute"),
    });

    /* ── Misc ── */
    auto misc = group(" Misc ", Elements{
        entry("r",        "Cycle loop mode"),
        entry("l",        "Toggle lyrics"),
        entry("?",        "Toggle this help"),
        entry("q / Esc",  "Quit"),
    });

    auto left_col  = vbox(Elements{ nav, text(""), play });
    auto right_col = vbox(Elements{ vol, text(""), misc });
    auto body = hbox(Elements{
        left_col  | flex,
        separator(),
        right_col | flex,
    });

    Elements col;
    col.push_back(body);
    col.push_back(separator());
    col.push_back(text(" Press ? again or Escape to close ") | dim | center);

    auto help_box = vbox(std::move(col));
    auto &theme = ThemeManager::instance().current();
    /* Full-page help: title sits above the bordered table, content
       vertically centered, scrolls when the window is too short
       (same pattern as login screen) */
    return vbox(Elements{
        filler(),
        text(" Help ") | bold | center,
        help_box | border,
        filler(),
    }) | yframe | flex |
        bgcolor(Color::RGB(theme.overlay_bg.r, theme.overlay_bg.g, theme.overlay_bg.b));
}
