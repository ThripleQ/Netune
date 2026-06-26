#include "keybindings.h"
#include "infra/log.h"
#include <yaml.h>
#include <cstring>
#include <unordered_map>
#include <cassert>

/* ── Internal implementation ────────────────────────── */
struct KeybindingManager::Impl {
    std::unordered_map<std::string, Action> map;
};

KeybindingManager::KeybindingManager() {
    /* default bindings — used if no YAML loaded */
    impl_ = new Impl;
    impl_->map["j"]      = Action::MoveDown;
    impl_->map["down"]   = Action::MoveDown;
    impl_->map["k"]      = Action::MoveUp;
    impl_->map["up"]     = Action::MoveUp;
    impl_->map["tab"]    = Action::PanelSwitch;
    impl_->map["space"]  = Action::PlayPause;
    impl_->map["enter"]  = Action::PlaySelected;
    impl_->map["n"]      = Action::NextTrack;
    impl_->map["p"]      = Action::PrevTrack;
    impl_->map["left"]   = Action::SeekBackward;
    impl_->map["right"]  = Action::SeekForward;
    impl_->map["+"]      = Action::VolumeUp;
    impl_->map["="]      = Action::VolumeUp;
    impl_->map["-"]      = Action::VolumeDown;
    impl_->map["l"]      = Action::CycleLoop;
    impl_->map["q"]      = Action::Quit;
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
    FILE *fp = fopen(yaml_path.c_str(), "rb");
    if (!fp) {
        LOG_WARN("Cannot open keybindings: %s", yaml_path.c_str());
        return false;
    }

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
