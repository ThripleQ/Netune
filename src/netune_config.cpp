/* netune-config — interactive settings editor for Netune.
   Sections: keybindings / theme / playback.
   Keybindings and theme changes are written back to the files Netune
   reads at startup (data/keybindings/default.yaml and data/config.json). */

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <unistd.h>

#include "infra/config.h"
#include "ui/keybindings.h"
#include <yaml.h>

using namespace ftxui;

/* ── Path helpers (mirrors app.cpp xdg_data_root) ──── */
static std::string data_root(void) {
    const char *d = getenv("XDG_CONFIG_HOME");
    if (d && d[0]) return std::string(d) + "/netune/data";
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/netune/data";
}

static std::string kb_path(void)  { return data_root() + "/keybindings/default.yaml"; }
static std::string cfg_path(void) { return data_root() + "/config.json"; }

/* ── Action metadata (display names) ────────────────── */
struct ActionInfo {
    Action act;
    const char *name;      /* yaml key */
    const char *desc;      /* zh display */
};

static const ActionInfo kActions[] = {
    {Action::MoveUp,       "move_up",        "向上移动"},
    {Action::MoveDown,     "move_down",      "向下移动"},
    {Action::PanelSwitch,  "panel_switch",   "切换面板 (Tab)"},
    {Action::PlayPause,    "play_pause",     "播放/暂停"},
    {Action::PlaySelected, "play_select",    "播放选中项"},
    {Action::NextTrack,    "next_track",     "下一首"},
    {Action::PrevTrack,    "prev_track",     "上一首"},
    {Action::SeekForward,  "seek_forward",   "快进"},
    {Action::SeekBackward, "seek_backward",  "快退"},
    {Action::VolumeUp,     "volume_up",      "音量+"},
    {Action::VolumeDown,   "volume_down",    "音量-"},
    {Action::ToggleMute,   "toggle_mute",    "静音"},
    {Action::CycleLoop,    "cycle_loop",     "循环模式"},
    {Action::ToggleLyrics, "toggle_lyrics",  "歌词"},
    {Action::OpenSearch,   "open_search",    "搜索 (Ctrl+/)"},
    {Action::Stop,         "stop",           "停止"},
    {Action::ShowHelp,     "show_help",      "帮助"},
    {Action::ShowActions,  "show_actions",   "操作小窗"},
    {Action::ShowSongDetail, "show_song_detail", "歌曲详情"},
    {Action::Quit,         "quit",           "退出"},
};

/* ── Key name conversion (mirrors app.cpp) ──────────── */
static std::string event_to_key_name(const Event &event) {
    /* Alt combos: terminal sends ESC + char (2 bytes) */
    if (event.input().size() == 2 &&
        (unsigned char)event.input()[0] == 0x1b &&
        (unsigned char)event.input()[1] >= 32 &&
        (unsigned char)event.input()[1] != 127) {
        return "alt+" + event.input().substr(1, 1);
    }
    if (event.input().size() == 1) {
        unsigned char c = (unsigned char)event.input()[0];
        if (c == 0x1f || c == 0x00) return "ctrl+/";
        if (c == 0x1c) return "ctrl+\\";
        if (c == 0x1d) return "ctrl+]";
        if (c == 0x1e) return "ctrl+^";
    }
    if (event == Event::ArrowUp)        return "up";
    if (event == Event::ArrowDown)      return "down";
    if (event == Event::ArrowLeft)      return "left";
    if (event == Event::ArrowRight)     return "right";
    if (event == Event::Backspace)      return "backspace";
    if (event == Event::Delete)         return "delete";
    if (event == Event::Return)         return "enter";
    if (event == Event::Escape)         return "escape";
    if (event == Event::Tab)            return "tab";
    if (event == Event::TabReverse)     return "tab_reverse";
    if (event == Event::F1)  return "f1";  if (event == Event::F2)  return "f2";
    if (event == Event::F3)  return "f3";  if (event == Event::F4)  return "f4";
    if (event == Event::F5)  return "f5";  if (event == Event::F6)  return "f6";
    if (event == Event::F7)  return "f7";  if (event == Event::F8)  return "f8";
    if (event == Event::F9)  return "f9";  if (event == Event::F10) return "f10";
    if (event == Event::F11) return "f11"; if (event == Event::F12) return "f12";
    if (event.is_character()) {
        std::string c = event.character();
        if (c.size() == 1 && (unsigned char)c[0] >= 32) {
            if (c == " ") return "space";
            return c;
        }
    }
    if (event.input().size() == 1) {
        unsigned char c = (unsigned char)event.input()[0];
        if (c >= 1 && c <= 26) {
            char buf[16];
            snprintf(buf, sizeof(buf), "ctrl+%c", 'a' + c - 1);
            return buf;
        }
        if (c == 127) return "delete";
    }
    return "";
}

/* ── Keybindings YAML writer (libyaml) ──────────────── */
static bool write_keybindings_yaml(const std::string &path,
                                   const std::vector<std::pair<std::string, std::vector<std::string>>> &entries) {
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) return false;

    yaml_emitter_t em;
    yaml_document_t doc;
    yaml_emitter_initialize(&em);
    yaml_emitter_set_output_file(&em, fp);
    yaml_emitter_set_encoding(&em, YAML_UTF8_ENCODING);
    yaml_document_initialize(&doc, NULL, NULL, NULL, 1, 1);

    int root = yaml_document_add_mapping(&doc, NULL, YAML_BLOCK_MAPPING_STYLE);
    int kb = yaml_document_add_mapping(&doc, NULL, YAML_BLOCK_MAPPING_STYLE);
    yaml_document_append_mapping_pair(&doc, root,
        yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)"keybindings", 12, YAML_PLAIN_SCALAR_STYLE),
        kb);

    for (auto &e : entries) {
        int seq = yaml_document_add_sequence(&doc, NULL, YAML_BLOCK_SEQUENCE_STYLE);
        for (auto &k : e.second) {
            yaml_document_append_sequence_item(&doc, seq,
                yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)k.c_str(), (int)k.size(), YAML_PLAIN_SCALAR_STYLE));
        }
        yaml_document_append_mapping_pair(&doc, kb,
            yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)e.first.c_str(), (int)e.first.size(), YAML_PLAIN_SCALAR_STYLE),
            seq);
    }

    yaml_emitter_dump(&em, &doc);
    yaml_document_delete(&doc);
    yaml_emitter_delete(&em);
    fclose(fp);
    return true;
}

/* ── State ──────────────────────────────────────────── */
struct CfgState {
    int section = 0;                       /* 0 keybindings, 1 theme, 2 playback */
    int kb_sel = 0;
    int theme_sel = 0;
    int play_sel = 0;
    bool capturing = false;                /* waiting for a keypress */
    bool dirty_kb = false;
    std::vector<std::pair<Action, std::vector<std::string>>> kb_map;  /* current live keys */
    std::vector<std::string> themes;
    std::string notice;
};

static std::string key_list_str(const std::vector<std::string> &keys) {
    std::string out;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) out += ", ";
        out += keys[i];
    }
    return out.empty() ? "(未绑定)" : out;
}

int main() {
    auto screen = ScreenInteractive::Fullscreen();

    CfgState st;

    /* load keybindings */
    {
        KeybindingManager km;
        km.load(kb_path());
        for (auto &a : kActions) {
            st.kb_map.push_back({a.act, km.keys_for(a.act)});
        }
    }

    /* list themes */
    {
        std::string dir = data_root() + "/themes";
        DIR *dp = opendir(dir.c_str());
        if (dp) {
            struct dirent *e;
            while ((e = readdir(dp))) {
                std::string n = e->d_name;
                if (n.size() > 5 && n.substr(n.size() - 5) == ".yaml")
                    st.themes.push_back(n.substr(0, n.size() - 5));
            }
            closedir(dp);
            std::sort(st.themes.begin(), st.themes.end());
        }
    }

    /* current config */
    Config *cfg = config_load(cfg_path().c_str());
    std::string cur_theme = cfg ? config_get_str(cfg, "ui.theme", "default") : "default";
    int cur_vol  = cfg ? config_get_int(cfg, "audio.volume", 80) : 80;
    int cur_loop = cfg ? config_get_int(cfg, "playback.loop_mode", 0) : 0;
    int cur_seek = cfg ? config_get_int(cfg, "playback.seek_step_sec", 5) : 5;
    if (cur_seek < 1) cur_seek = 1;
    if (cur_seek > 60) cur_seek = 60;
    int vol = cur_vol, loop_mode = cur_loop, seek = cur_seek;

    auto theme_sel_matches = [&](const std::string &name) {
        auto it = std::find(st.themes.begin(), st.themes.end(), name);
        st.theme_sel = (it != st.themes.end()) ? (int)(it - st.themes.begin()) : 0;
    };
    theme_sel_matches(cur_theme);

    /* ── Renderer ───────────────────────────────────── */
    auto renderer = Renderer([&] {
        auto &th = st;

        /* left: sections */
        std::vector<std::string> sections = {"快捷键", "主题", "播放"};
        Elements left;
        for (size_t i = 0; i < sections.size(); i++) {
            bool sel = ((int)i == th.section);
            auto row = text(sections[i]);
            if (sel) left.push_back(hbox({text("> "), row | bold}) | inverted);
            else     left.push_back(hbox({text("  "), row}));
        }
        left.push_back(text(""));
        left.push_back(text(" q/ESC 保存并退出") | dim);
        left.push_back(text(" ↑/↓ 选择  Enter 编辑") | dim);

        /* right: section content */
        Elements right;
        if (th.capturing) {
            right.push_back(text("  编辑按键: 按新键=绑定(支持 ctrl+/alt 组合), 已有键=取消") | bold);
            right.push_back(text("  当前: " + key_list_str(st.kb_map[st.kb_sel].second)));
            right.push_back(text("  Backspace 删除最后一个  Enter 完成  ESC 取消") | dim);
        } else if (th.section == 0) {
            for (size_t i = 0; i < sizeof(kActions)/sizeof(kActions[0]); i++) {
                bool sel = ((int)i == th.kb_sel);
                std::string line = std::string("  ") + kActions[i].desc + "  =  " + key_list_str(st.kb_map[i].second);
                if (sel)
                    right.push_back(hbox({text("> "), text(line) | bold}) | inverted);
                else
                    right.push_back(text(line));
            }
            right.push_back(text(""));
            right.push_back(text("  Enter: 重新绑定该操作") | dim);
        } else if (th.section == 1) {
            for (size_t i = 0; i < st.themes.size(); i++) {
                bool sel = ((int)i == th.theme_sel);
                std::string mark = (st.themes[i] == cur_theme) ? "  (当前)" : "";
                std::string line = std::string("  ") + st.themes[i] + mark;
                if (sel)
                    right.push_back(hbox({text("> "), text(line) | bold}) | inverted);
                else
                    right.push_back(text(line));
            }
            right.push_back(text(""));
            right.push_back(text("  Enter: 应用该主题 (重启后生效)") | dim);
        } else {
            const char *loops[] = {"顺序播放", "单曲循环", "列表循环", "随机播放"};
            int lv = st.play_sel;
            auto row_vol = [&](int v) { return std::string("  音量:  ") + std::to_string(v) + "   [<- / ->]"; };
            auto row_loop = [&](int v) { return std::string("  循环:  ") + loops[v % 4] + "   [l 切换]"; };
            auto row_seek = [&](int v) { return std::string("  快进步长: ") + std::to_string(v) + " 秒  [+ / -]"; };
            right.push_back(text(row_vol(vol)));
            right.push_back(text(row_loop(loop_mode)));
            right.push_back(text(row_seek(seek)));
            right.push_back(text(""));
            right.push_back(text("  修改立即保存到 config.json") | dim);
        }

        if (!th.notice.empty()) {
            right.push_back(text(""));
            right.push_back(text("  " + th.notice) | color(Color::Green));
        }

        return hbox({
            vbox(std::move(left)) | size(WIDTH, EQUAL, 26) | border,
            vbox(std::move(right)) | flex | border,
        });
    });

    /* ── Event handling ─────────────────────────────── */
    Component main = renderer;
    main |= CatchEvent([&](Event event) -> bool {
        if (event.is_mouse()) return true;

        if (st.capturing) {
            if (event == Event::Escape) {
                st.capturing = false;
                return true;
            }
            if (event == Event::Return || event.character() == "\r") {
                st.capturing = false;  /* Enter: finish editing */
                return true;
            }
            if (event == Event::Backspace) {
                /* delete the last bound key, one at a time */
                auto &keys = st.kb_map[st.kb_sel].second;
                if (!keys.empty()) {
                    st.notice = "已删除 " + keys.back();
                    keys.pop_back();
                    st.dirty_kb = true;
                }
                return true;
            }
            std::string k = event_to_key_name(event);
            if (!k.empty() && k != "enter") {
                std::vector<std::string> &keys = st.kb_map[st.kb_sel].second;
                /* avoid duplicates across actions */
                for (auto &kv : st.kb_map) {
                    auto it = std::find(kv.second.begin(), kv.second.end(), k);
                    if (it != kv.second.end() &&
                        std::addressof(kv.second) != std::addressof(keys))
                        kv.second.erase(it);
                }
                auto it = std::find(keys.begin(), keys.end(), k);
                if (it == keys.end()) {
                    keys.push_back(k);
                    st.notice = "已绑定 " + k + " (Enter 完成 / 再按一次可取消)";
                } else {
                    keys.erase(it);
                    st.notice = "已取消 " + k + " (Enter 完成)";
                }
                st.dirty_kb = true;
                /* stay in capture mode: keep binding/unbinding keys */
            }
            return true;
        }

        std::string k;
        if (event.is_character()) k = event.character();
        else k = event_to_key_name(event);

        if (k == "q" || k == "ctrl+c" || k == "escape") {
            if (st.section == 0 && st.dirty_kb) {
                std::vector<std::pair<std::string, std::vector<std::string>>> entries;
                for (auto &kv : st.kb_map) {
                    for (auto &a : kActions) {
                        if (a.act == kv.first) {
                            entries.push_back({a.name, kv.second});
                            break;
                        }
                    }
                }
                write_keybindings_yaml(kb_path(), entries);
            }
            if (cfg) {
                config_set_int(cfg, "audio.volume", vol);
                config_set_int(cfg, "playback.loop_mode", loop_mode);
                config_set_int(cfg, "playback.seek_step_sec", seek);
                config_set_str(cfg, "ui.theme", st.themes[st.theme_sel].c_str());
                config_save(cfg);
                config_free(cfg);
            }
            screen.ExitLoopClosure()();
            return true;
        }
        if (k == "up" || k == "down" || k == "j" || k == "k") {
            int dir = (k == "up" || k == "k") ? -1 : 1;
            if (st.section == 0) {
                int n = (int)(sizeof(kActions)/sizeof(kActions[0]));
                st.kb_sel = (st.kb_sel + dir + n) % n;
            } else if (st.section == 1) {
                int n = (int)st.themes.size();
                if (n > 0) st.theme_sel = (st.theme_sel + dir + n) % n;
            }
            return true;
        }
        if (k == "left" || k == "right" || k == "tab") {
            if (k == "tab") {
                st.section = (st.section + 1) % 3;
            } else if (st.section == 2) {
                if (k == "left") vol = vol > 0 ? vol - 5 : 0;
                else             vol = vol < 100 ? vol + 5 : 100;
            }
            return true;
        }
        if (k == "l" && st.section == 2) {
            loop_mode = (loop_mode + 1) % 4;
            return true;
        }
        if (k == "+" || k == "=" || k == "-") {
            if (st.section == 2) {
                if (k == "-") seek = seek > 1 ? seek - 1 : 1;
                else          seek = seek < 60 ? seek + 1 : 60;
            }
            return true;
        }
        if (k == "enter" || k == "\r") {
            if (st.section == 0) {
                st.capturing = true;
                st.notice.clear();
            } else if (st.section == 1 && !st.themes.empty()) {
                cur_theme = st.themes[st.theme_sel];
                st.notice = "已选择主题: " + cur_theme + " (重启 netune 生效)";
            }
            return true;
        }
        return true;
    });

    ftxui::Loop loop(&screen, main);
    loop.Run();
    screen.ExitLoopClosure()();
    return 0;
}
