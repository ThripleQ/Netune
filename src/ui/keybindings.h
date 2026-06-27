#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>

/* ── All user actions ─────────────────────────────── */
enum class Action {
    None,
    MoveUp, MoveDown,
    PanelSwitch,
    PlayPause,
    PlaySelected,
    NextTrack, PrevTrack,
    Stop,
    SeekForward, SeekBackward,
    VolumeUp, VolumeDown,
    ToggleMute,
    CycleLoop,
    ShowHelp,
    Quit,
};

/* ── Key combo (single key that maps to an action) ─── */
struct KeyCombo {
    std::string key;   /* "j", "down", "space", "enter", "tab", "q" */
};

/* ── Manager ──────────────────────────────────────────
   Loads YAML config like:
     keybindings:
       move_up:    ["up", "k"]
       move_down:  ["down", "j"]
       play_pause: ["space"]
       quit:       ["q", "ctrl+c"]
   Then lookup("j") → MoveDown.
   Multiple keys can map to the same action.
   Each key maps to exactly one action (first match wins).
   ───────────────────────────────────────────────── */
class KeybindingManager {
public:
    KeybindingManager();
    ~KeybindingManager();
    bool load(const std::string &yaml_path);
    std::optional<Action> lookup(const std::string &key) const;

private:
    /* shared state — one manager owns the mappings */
    struct Impl;
    Impl *impl_ = nullptr;
};
