#include "ui/components/help_screen.h"
#include "ui/components/theme_util.h"
#include "ui/theme.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
using namespace ftxui;

/* ── Entry table: action → description, grouped ───────── */
struct HelpEntry {
    Action      action;
    const char *desc;
};

static const HelpEntry kNav[] = {
    {Action::PanelSwitch,  "Switch panel (groups / songs)"},
    {Action::MoveDown,     "Move down"},
    {Action::MoveUp,       "Move up"},
    {Action::PlaySelected, "Play selected song"},
    {Action::OpenSearch,   "Search"},
};

static const HelpEntry kPlay[] = {
    {Action::PlayPause,    "Play / Pause"},
    {Action::NextTrack,    "Next track"},
    {Action::PrevTrack,    "Previous track"},
    {Action::SeekForward,  "Seek forward"},
    {Action::SeekBackward, "Seek backward"},
    {Action::Stop,         "Stop playback"},
};

static const HelpEntry kVol[] = {
    {Action::VolumeUp,     "Volume up"},
    {Action::VolumeDown,   "Volume down"},
    {Action::ToggleMute,   "Toggle mute"},
};

static const HelpEntry kMisc[] = {
    {Action::CycleLoop,    "Cycle loop mode"},
    {Action::ToggleLyrics, "Toggle lyrics"},
    {Action::ShowHelp,     "Toggle this help"},
    {Action::ShowActions,  "Like song / subscribe playlist"},
    {Action::ShowSongDetail, "Show song detail popup"},
    {Action::Quit,         "Quit"},
};

/* ── Key name display (raw config strings → pretty names) ── */
static std::string key_display(const std::string &k) {
    if (k == "down")      return "Down";
    if (k == "up")        return "Up";
    if (k == "space")     return "Space";
    if (k == "enter")     return "Enter";
    if (k == "tab")       return "Tab";
    if (k == "escape")    return "Esc";
    if (k == "left")      return "Left";
    if (k == "right")     return "Right";
    if (k == "ctrl+c")    return "Ctrl+C";
    return k;
}

Element render_help_screen(const AppState &s, const KeybindingManager &kb) {
    (void)s;

    /* Key column width — all keys are ASCII so manual padding is safe */
    constexpr int KEYW = 12;

    /* Resolve bound keys for an action, joined as "j / Down".
       Sorted for a stable display order. */
    auto action_keys = [&](Action a) {
        auto keys = kb.keys_for(a);
        for (auto &k : keys) k = key_display(k);
        std::sort(keys.begin(), keys.end());
        std::string out;
        for (size_t i = 0; i < keys.size(); i++) {
            if (i) out += " / ";
            out += keys[i];
        }
        if (out.empty()) out = "—";  /* unbound */
        return out;
    };

    auto entry = [&](const HelpEntry &he) {
        size_t klen = action_keys(he.action).size();
        size_t pad = klen < KEYW ? KEYW - klen : 1;
        return hbox(Elements{
            text("  "),
            theme_fg(text(action_keys(he.action) + std::string(pad, ' ')) | bold),
            theme_fg(text(he.desc)),
        });
    };

    auto group = [&](const char *title, const HelpEntry *items, size_t n) {
        Elements col;
        col.push_back(theme_fg(text(title) | bold));
        col.push_back(text(""));
        for (size_t i = 0; i < n; i++)
            col.push_back(entry(items[i]));
        return vbox(std::move(col));
    };

    auto nav  = group(" Navigation ", kNav,  sizeof(kNav)  / sizeof(kNav[0]));
    auto play = group(" Playback ",   kPlay, sizeof(kPlay) / sizeof(kPlay[0]));
    auto vol  = group(" Volume ",     kVol,  sizeof(kVol)  / sizeof(kVol[0]));
    auto misc = group(" Misc ",       kMisc, sizeof(kMisc) / sizeof(kMisc[0]));

    /* Two columns; the filler inside each column pushes the two groups to
       top and bottom so the columns balance visually */
    auto left_col  = vbox(Elements{ nav, filler(), play });
    auto right_col = vbox(Elements{ vol, filler(), misc });
    auto body = hbox(Elements{
        left_col  | flex,
        text("   "),
        right_col | flex,
    });

    auto &theme = ThemeManager::instance().current();
    /* Full-page help: title above the bordered table, hint below it,
       content vertically centered, scrolls when the window is too short
       (same pattern as login screen) */
    return vbox(Elements{
        filler(),
        theme_accent(text(" Help ") | bold),
        body | border,
        text(" Press ? again or Escape to close ") | dim | center,
        filler(),
    }) | yframe | flex |
        bgcolor(Color::RGB(theme.overlay_bg.r, theme.overlay_bg.g, theme.overlay_bg.b));
}
