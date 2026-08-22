#include "keybindings.h"
#include "infra/log.h"
#include <yaml.h>
#include <cstring>
#include <unordered_map>
#include <cassert>
#include "compat/utf8.h"   /* UTF-8 aware fopen for Windows */

/* ── Internal implementation ────────────────────────── */
struct KeybindingManager::Impl {
    std::unordered_map<std::string, Action> map;
};

/* default bindings — base map, YAML overrides on top */
static void fill_defaults(std::unordered_map<std::string, Action> &map) {
    map["j"]      = Action::MoveDown;
    map["down"]   = Action::MoveDown;
    map["k"]      = Action::MoveUp;
    map["up"]     = Action::MoveUp;
    map["tab"]    = Action::PanelSwitch;
    map["space"]  = Action::PlayPause;
    map["enter"]  = Action::PlaySelected;
    map["n"]      = Action::NextTrack;
    map["p"]      = Action::PrevTrack;
    map["left"]   = Action::SeekBackward;
    map["right"]  = Action::SeekForward;
    map["+"]      = Action::VolumeUp;
    map["="]      = Action::VolumeUp;
    map["-"]      = Action::VolumeDown;
    map["r"]      = Action::CycleLoop;
    map["l"]      = Action::ToggleLyrics;
    map["s"]      = Action::Stop;
    map["m"]      = Action::ToggleMute;
    map["?"]      = Action::ShowHelp;
    map["ctrl+x"] = Action::ShowActions;
    map["ctrl+/"] = Action::OpenSearch;
    map["q"]      = Action::Quit;
}

KeybindingManager::KeybindingManager() {
    /* default bindings — used if no YAML loaded */
    impl_ = new Impl;
    fill_defaults(impl_->map);
}

KeybindingManager::~KeybindingManager() {
    delete impl_;
}

/* ── YAML parsing helper ──────────────────────────────── */
/* We parse a very constrained subset of YAML:
     keybindings:
       action_name: ["key1", "key2"]
       ...
*/
static const char *yaml_scalar(yaml_event_t *ev) {
    return (ev->type == YAML_SCALAR_EVENT) ? (const char*)ev->data.scalar.value : nullptr;
}

bool KeybindingManager::load(const std::string &yaml_path) {
    FILE *fp = fopen_utf8(yaml_path.c_str(), "rb");
    if (!fp) {
        LOG_WARN("Cannot open keybindings: %s", yaml_path.c_str());
        return false;
    }

    /* start from defaults; YAML keys override/remove them.  Without the
       clear, keys removed from the YAML (e.g. "escape" → ShowHelp) would
       linger forever from the constructor's default map. */
    impl_->map.clear();
    fill_defaults(impl_->map);

    yaml_parser_t parser;
    yaml_event_t  event;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, fp);

    bool in_keybindings = false;
    bool in_list = false;
    std::string current_action_name;
    Action current_action = Action::None;

    /* helper: convert action name string → Action enum */
    auto name_to_action = [](const std::string &name) -> Action {
        if (name == "move_up")       return Action::MoveUp;
        if (name == "move_down")     return Action::MoveDown;
        if (name == "panel_switch")  return Action::PanelSwitch;
        if (name == "play_pause")    return Action::PlayPause;
        if (name == "play_select")   return Action::PlaySelected;
        if (name == "next_track")    return Action::NextTrack;
        if (name == "prev_track")    return Action::PrevTrack;
        if (name == "seek_forward")  return Action::SeekForward;
        if (name == "seek_backward") return Action::SeekBackward;
        if (name == "volume_up")     return Action::VolumeUp;
        if (name == "volume_down")   return Action::VolumeDown;
        if (name == "cycle_loop")    return Action::CycleLoop;
        if (name == "toggle_lyrics") return Action::ToggleLyrics;
        if (name == "stop")          return Action::Stop;
        if (name == "toggle_mute")   return Action::ToggleMute;
        if (name == "open_search")   return Action::OpenSearch;
        if (name == "show_help")     return Action::ShowHelp;
        if (name == "show_actions")  return Action::ShowActions;
        if (name == "quit")          return Action::Quit;
        return Action::None;
    };

    while (yaml_parser_parse(&parser, &event)) {
        if (event.type == YAML_STREAM_END_EVENT) {
            yaml_event_delete(&event);
            break;
        }

        if (event.type == YAML_SCALAR_EVENT) {
            const char *val = yaml_scalar(&event);
            if (!val) { yaml_event_delete(&event); continue; }

            if (in_list) {
                /* val is a key string */
                if (current_action != Action::None) {
                    impl_->map[val] = current_action;
                    LOG_DEBUG("Keybind: '%s' → %d", val, (int)current_action);
                }
            } else if (strcmp(val, "keybindings") == 0) {
                in_keybindings = true;
            } else if (in_keybindings) {
                current_action = name_to_action(val);
                if (current_action == Action::None)
                    LOG_WARN("Unknown action: %s", val);
            }
        }

        if (event.type == YAML_SEQUENCE_START_EVENT) {
            in_list = in_keybindings;
        }
        if (event.type == YAML_SEQUENCE_END_EVENT) {
            in_list = false;
            current_action = Action::None;
        }
        if (event.type == YAML_MAPPING_END_EVENT && in_keybindings) {
            in_keybindings = false;
        }

        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(fp);
    LOG_INFO("Keybindings loaded: %s", yaml_path.c_str());
    return true;
}

std::optional<Action> KeybindingManager::lookup(const std::string &key) const {
    if (!impl_) return std::nullopt;
    auto it = impl_->map.find(key);
    if (it != impl_->map.end())
        return it->second;
    return std::nullopt;
}

std::vector<std::string> KeybindingManager::keys_for(Action action) const {
    std::vector<std::string> out;
    if (!impl_) return out;
    for (const auto &kv : impl_->map) {
        if (kv.second == action)
            out.push_back(kv.first);
    }
    return out;
}
