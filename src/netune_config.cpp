/* netune-config — interactive settings editor for Netune.
   Sections: keybindings / theme / playback.
   - Keybindings: list all actions, capture new keys (ctrl/alt combos),
     Backspace removes keys one by one; export/import the yaml.
   - Theme: pick a theme, edit each of the 14 color slots (hex input +
     preset palette); export/import theme files.
   - Playback: volume / loop mode / seek step.
   Everything is written back to the files Netune reads at startup. */

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
#include "ui/theme.h"
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
static std::string themes_dir(void) { return data_root() + "/themes"; }

/* ── Action metadata ────────────────────────────────── */
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

/* ── Theme color slots ──────────────────────────────── */
struct ColorSlot {
    const char *key;
    const char *name;
    ThemeColor Theme::*member;
};

static const ColorSlot kSlots[] = {
    {"bg",             "背景",      &Theme::bg},
    {"fg",             "文字",      &Theme::fg},
    {"accent",         "强调色",    &Theme::accent},
    {"accent_bg",      "选中背景",  &Theme::accent_bg},
    {"muted",          "次要文字",  &Theme::muted},
    {"border",         "边框",      &Theme::border},
    {"success",        "成功",      &Theme::success},
    {"warning",        "警告",      &Theme::warning},
    {"error",          "错误",      &Theme::error},
    {"overlay_bg",     "弹窗背景",  &Theme::overlay_bg},
    {"progress_track", "进度条轨道", &Theme::progress_track},
    {"spectrum",       "频谱",      &Theme::spectrum},
    {"vip",            "VIP 标记",  &Theme::vip},
    {"playlist",       "歌单标记",  &Theme::playlist},
};

/* ── Preset palette (16 colors) ─────────────────────── */
static const char *kPalette[] = {
    "#000000", "#37474f", "#888888", "#b0bec5", "#ffffff",
    "#e53935", "#fb8c00", "#fdd835", "#43a047", "#00acc1",
    "#1e88e5", "#8e24aa", "#e91e63", "#795548", "#9ece6a", "#f7768e",
};
static const int kPaletteN = (int)(sizeof(kPalette)/sizeof(kPalette[0]));

/* ── Modes ──────────────────────────────────────────── */
enum class Mode { Normal, Capture, ColorEdit, PathInput, Confirm };

struct CfgState {
    Mode mode = Mode::Normal;

    int section = 0;                       /* 0 keybindings, 1 theme, 2 playback */
    int kb_sel = 0;
    int theme_sel = 0;
    int slot_sel = 0;
    int palette_sel = 0;

    bool dirty_kb = false;
    bool dirty_theme = false;

    std::vector<std::pair<Action, std::vector<std::string>>> kb_map;
    std::vector<std::string> themes;

    /* color edit */
    std::string hex_buf;

    /* path input: import/export */
    std::string path_buf;
    bool path_import = false;   /* false = export */
    bool path_for_theme = false;

    std::string notice;
    std::string theme_name;
    Theme theme;
};

static std::string key_list_str(const std::vector<std::string> &keys) {
    std::string out;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) out += ", ";
        out += keys[i];
    }
    return out.empty() ? "(未绑定)" : out;
}

static std::string basename_of(const std::string &p) {
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

static bool copy_file(const std::string &src, const std::string &dst) {
    FILE *in = fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE *out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return true;
}

/* ── Key name conversion (mirrors app.cpp) ──────────── */
static std::string event_to_key_name(const Event &event) {
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

/* ── YAML writers (libyaml) ─────────────────────────── */

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
        for (auto &key : e.second) {
            yaml_document_append_sequence_item(&doc, seq,
                yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)key.c_str(), (int)key.size(), YAML_PLAIN_SCALAR_STYLE));
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

static bool write_theme_yaml(const std::string &path, const std::string &name, const Theme &t) {
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) return false;

    yaml_emitter_t em;
    yaml_document_t doc;
    yaml_emitter_initialize(&em);
    yaml_emitter_set_output_file(&em, fp);
    yaml_emitter_set_encoding(&em, YAML_UTF8_ENCODING);
    yaml_document_initialize(&doc, NULL, NULL, NULL, 1, 1);

    int root = yaml_document_add_mapping(&doc, NULL, YAML_BLOCK_MAPPING_STYLE);
    int colors = yaml_document_add_mapping(&doc, NULL, YAML_BLOCK_MAPPING_STYLE);
    yaml_document_append_mapping_pair(&doc, root,
        yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)"name", 4, YAML_PLAIN_SCALAR_STYLE),
        yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)name.c_str(), (int)name.size(), YAML_PLAIN_SCALAR_STYLE));
    yaml_document_append_mapping_pair(&doc, root,
        yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)"colors", 6, YAML_PLAIN_SCALAR_STYLE),
        colors);

    for (auto &s : kSlots) {
        const ThemeColor &c = t.*(s.member);
        std::string hex = theme_color_to_hex(c);
        yaml_document_append_mapping_pair(&doc, colors,
            yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)s.key, (int)strlen(s.key), YAML_PLAIN_SCALAR_STYLE),
            yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)hex.c_str(), (int)hex.size(), YAML_PLAIN_SCALAR_STYLE));
    }

    yaml_emitter_dump(&em, &doc);
    yaml_document_delete(&doc);
    yaml_emitter_delete(&em);
    fclose(fp);
    return true;
}

/* ── Theme loading helper ───────────────────────────── */
static bool load_theme(CfgState &st, const std::string &name) {
    std::string path = themes_dir() + "/" + name + ".yaml";
    auto &tm = ThemeManager::instance();
    tm.reset();
    if (!tm.load(path)) return false;
    st.theme = tm.current();
    st.theme_name = name;
    return true;
}

/* ── Helpers used by event handling ─────────────────── */
static std::vector<std::pair<std::string, std::vector<std::string>>> kb_entries(CfgState &st) {
    std::vector<std::pair<std::string, std::vector<std::string>>> entries;
    for (auto &kv : st.kb_map) {
        for (auto &a : kActions) {
            if (a.act == kv.first) {
                entries.push_back({a.name, kv.second});
                break;
            }
        }
    }
    return entries;
}

static void refresh_theme_list(CfgState &st) {
    st.themes.clear();
    DIR *dp = opendir(themes_dir().c_str());
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

static bool yaml_validate(const std::string &path, const std::string &key) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    yaml_parser_t parser;
    yaml_event_t event;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, fp);
    bool found = false;
    while (yaml_parser_parse(&parser, &event)) {
        if (event.type == YAML_STREAM_END_EVENT) { yaml_event_delete(&event); break; }
        if (event.type == YAML_SCALAR_EVENT &&
            (const char*)event.data.scalar.value &&
            strcmp((const char*)event.data.scalar.value, key.c_str()) == 0) {
            found = true;
            yaml_event_delete(&event);
            break;
        }
        yaml_event_delete(&event);
    }
    yaml_parser_delete(&parser);
    fclose(fp);
    return found;
}

int main() {
    auto screen = ScreenInteractive::Fullscreen();
    CfgState st;

    /* load keybindings */
    {
        KeybindingManager km;
        km.load(kb_path());
        for (auto &a : kActions)
            st.kb_map.push_back({a.act, km.keys_for(a.act)});
    }

    refresh_theme_list(st);

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
    if (!load_theme(st, st.themes.empty() ? "default" : st.themes[st.theme_sel]))
        st.theme = Theme{};

    /* ── Renderer ───────────────────────────────────── */
    auto renderer = Renderer([&] {
        auto &th = st;
        const char *sections[] = {"快捷键", "主题", "播放"};

        Elements left;
        for (int i = 0; i < 3; i++) {
            bool sel = (th.section == i);
            auto row = text(sections[i]);
            if (sel) left.push_back(hbox({text("> "), row | bold}) | inverted);
            else     left.push_back(hbox({text("  "), row}));
        }
        left.push_back(text(""));
        left.push_back(text(" q/ESC 保存并退出") | dim);
        left.push_back(text(" ↑/↓ 选择  Enter 编辑") | dim);
        left.push_back(text(" e 导出  i 导入") | dim);

        Elements right;
        if (th.mode == Mode::Capture) {
            right.push_back(text("  编辑按键: 按新键=绑定(支持 ctrl/alt), 已有键=取消") | bold);
            right.push_back(text("  当前: " + key_list_str(th.kb_map[th.kb_sel].second)));
            right.push_back(text("  Backspace 删除最后一个  Enter 完成  ESC 取消") | dim);
        } else if (th.mode == Mode::ColorEdit) {
            const ColorSlot &slot = kSlots[th.slot_sel];
            ThemeColor &c = th.theme.*(slot.member);
            right.push_back(text(std::string("  编辑颜色 [") + slot.name + "]  当前 " +
                                 theme_color_to_hex(c)) | bold);
            auto swatch = text("  ") | bgcolor(Color::RGB(c.r, c.g, c.b));
            right.push_back(hbox({swatch, text("  输入 hex (如 #1a1b26): "),
                                  text(th.hex_buf + "\u258C")}));
            Elements pal;
            for (int i = 0; i < kPaletteN; i++) {
                ThemeColor pc = theme_color_from_hex(kPalette[i]);
                auto p = text("  ");
                if (i == th.palette_sel)
                    p = p | bold | inverted;
                pal.push_back(p | bgcolor(Color::RGB(pc.r, pc.g, pc.b)));
            }
            right.push_back(hbox(std::move(pal)));
            right.push_back(text("  ←/→ 选色板  Enter 应用  ESC 取消") | dim);
        } else if (th.mode == Mode::PathInput) {
            right.push_back(text(std::string("  ") +
                                 (th.path_import ? "导入" : "导出") +
                                 (th.path_for_theme ? "主题" : "按键配置") +
                                 ": 输入路径") | bold);
            right.push_back(text("  " + th.path_buf + "\u258C"));
            right.push_back(text("  空路径默认 ~/Downloads/  Enter 确认  ESC 取消") | dim);
        } else if (th.mode == Mode::Confirm) {
            right.push_back(text("  导入将覆盖当前按键配置, 继续?") | bold);
            right.push_back(text("  y 确认  n/ESC 取消") | dim);
        } else if (th.section == 0) {
            for (size_t i = 0; i < sizeof(kActions)/sizeof(kActions[0]); i++) {
                bool sel = ((int)i == th.kb_sel);
                std::string line = std::string("  ") + kActions[i].desc + "  =  " +
                                   key_list_str(th.kb_map[i].second);
                if (sel)
                    right.push_back(hbox({text("> "), text(line) | bold}) | inverted);
                else
                    right.push_back(text(line));
            }
            right.push_back(text(""));
            right.push_back(text("  Enter: 重新绑定  e: 导出  i: 导入") | dim);
        } else if (th.section == 1) {
            /* theme list (top) */
            right.push_back(text("  ── 主题 ──") | bold);
            for (size_t i = 0; i < th.themes.size(); i++) {
                bool sel = ((int)i == th.theme_sel);
                std::string mark = (th.themes[i] == cur_theme) ? "  (当前)" : "";
                std::string line = std::string("  ") + th.themes[i] + mark;
                if (sel)
                    right.push_back(hbox({text("> "), text(line) | bold}) | inverted);
                else
                    right.push_back(text(line));
            }
            right.push_back(text(""));
            /* color slots (bottom) */
            right.push_back(text("  ── 颜色槽 (选中主题) ──") | bold);
            for (size_t i = 0; i < sizeof(kSlots)/sizeof(kSlots[0]); i++) {
                const ColorSlot &slot = kSlots[i];
                const ThemeColor &c = th.theme.*(slot.member);
                bool sel = ((int)i == th.slot_sel && th.theme_sel >= 0);
                auto swatch = text("  ") | bgcolor(Color::RGB(c.r, c.g, c.b));
                std::string line = std::string("  ") + slot.name + "  " +
                                   theme_color_to_hex(c);
                Element row = hbox({swatch, text(line)});
                if (sel)
                    right.push_back(hbox({text("> "), row | bold}) | inverted);
                else
                    right.push_back(hbox({text("  "), row}));
            }
            right.push_back(text(""));
            right.push_back(text("  Enter: 编辑颜色/应用主题  e: 导出  i: 导入") | dim);
        } else {
            const char *loops[] = {"顺序播放", "单曲循环", "列表循环", "随机播放"};
            right.push_back(text(std::string("  音量:  ") + std::to_string(vol) +
                                 "   [<- / ->]"));
            right.push_back(text(std::string("  循环:  ") + loops[loop_mode % 4] +
                                 "   [l 切换]"));
            right.push_back(text(std::string("  快进步长: ") + std::to_string(seek) +
                                 " 秒  [+ / -]"));
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

        auto key_of = [&]() -> std::string {
            if (event.is_character()) return event.character();
            return event_to_key_name(event);
        };

        /* ── Color edit mode ── */
        if (st.mode == Mode::ColorEdit) {
            std::string k = key_of();
            if (event == Event::Escape) { st.mode = Mode::Normal; return true; }
            if (event == Event::ArrowLeft || k == "left") {
                st.palette_sel = (st.palette_sel + kPaletteN - 1) % kPaletteN;
                return true;
            }
            if (event == Event::ArrowRight || k == "right") {
                st.palette_sel = (st.palette_sel + 1) % kPaletteN;
                return true;
            }
            if (event == Event::Backspace) {
                if (!st.hex_buf.empty()) st.hex_buf.pop_back();
                return true;
            }
            if (event == Event::Return || k == "\r") {
                /* apply: hex buffer takes priority, else palette */
                ThemeColor nc;
                std::string hex = st.hex_buf;
                if (!hex.empty() && hex[0] != '#') hex = "#" + hex;
                bool ok = !hex.empty() && theme_color_from_hex(hex).has_color;
                if (ok) {
                    nc = theme_color_from_hex(hex);
                } else {
                    nc = theme_color_from_hex(kPalette[st.palette_sel]);
                    ok = nc.has_color;
                }
                if (ok) {
                    st.theme.*(kSlots[st.slot_sel].member) = nc;
                    st.dirty_theme = true;
                    st.notice = std::string("已设置 ") + kSlots[st.slot_sel].name +
                                " = " + theme_color_to_hex(nc);
                } else {
                    st.notice = "无效的 hex 颜色";
                }
                st.hex_buf.clear();
                st.mode = Mode::Normal;
                return true;
            }
            /* hex chars: # 0-9 a-f A-F */
            if (k.size() == 1 && st.hex_buf.size() < 9) {
                char c = k[0];
                if (c == '#' || (c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                    st.hex_buf += c;
            }
            return true;
        }

        /* ── Path input mode (import/export) ── */
        if (st.mode == Mode::PathInput) {
            std::string k = key_of();
            if (event == Event::Escape) { st.mode = Mode::Normal; return true; }
            if (event == Event::Backspace) {
                if (!st.path_buf.empty()) st.path_buf.pop_back();
                return true;
            }
            if (event == Event::Return || k == "\r") {
                std::string target = st.path_buf;
                if (target.empty()) {
                    const char *home = getenv("HOME");
                    target = std::string(home ? home : "~") + "/Downloads/" +
                             (st.path_for_theme ? st.theme_name : "keybindings") + ".yaml";
                }
                bool ok = false;
                if (!st.path_import) {
                    /* export */
                    if (st.path_for_theme) {
                        std::string src = themes_dir() + "/" + st.theme_name + ".yaml";
                        ok = copy_file(src, target);
                        if (!ok) ok = write_theme_yaml(target, st.theme_name, st.theme);
                    } else {
                        ok = write_keybindings_yaml(target, kb_entries(st));
                    }
                    st.notice = ok ? ("已导出: " + target) : "导出失败";
                } else {
                    /* import */
                    if (st.path_for_theme) {
                        if (!yaml_validate(target, "colors")) {
                            st.notice = "不是有效的主题文件 (缺少 colors)";
                        } else {
                            std::string base = basename_of(target);
                            if (base.size() > 5 && base.substr(base.size() - 5) == ".yaml")
                                base = base.substr(0, base.size() - 5);
                            if (base.empty()) base = "imported";
                            std::string dst = themes_dir() + "/" + base + ".yaml";
                            ok = copy_file(target, dst);
                            if (ok) {
                                refresh_theme_list(st);
                                auto it = std::find(st.themes.begin(), st.themes.end(), base);
                                st.theme_sel = (it != st.themes.end()) ? (int)(it - st.themes.begin()) : 0;
                                cur_theme = base;
                                load_theme(st, base);
                                st.notice = "已导入主题: " + base;
                            } else {
                                st.notice = "导入失败";
                            }
                        }
                    } else {
                        /* keybindings import → confirm first */
                        if (!yaml_validate(target, "keybindings")) {
                            st.notice = "不是有效的按键配置 (缺少 keybindings)";
                        } else {
                            st.notice = "确认导入? 将覆盖当前按键配置 (y/n)";
                            st.mode = Mode::Confirm;
                            return true;
                        }
                    }
                }
                st.mode = Mode::Normal;
                return true;
            }
            /* path chars: printable ASCII (incl space) */
            if (event.is_character() && st.path_buf.size() < 400) {
                const std::string &c = event.character();
                if (!c.empty()) st.path_buf += c;
            }
            return true;
        }

        /* ── Confirm mode (keybindings import) ── */
        if (st.mode == Mode::Confirm) {
            std::string k = key_of();
            if (k == "y" || k == "Y") {
                if (copy_file(st.path_buf, kb_path())) {
                    st.notice = "按键配置已导入 (重启 netune 生效)";
                } else {
                    st.notice = "导入失败";
                }
                st.mode = Mode::Normal;
                return true;
            }
            if (k == "n" || k == "N" || event == Event::Escape) {
                st.notice = "已取消导入";
                st.mode = Mode::Normal;
                return true;
            }
            return true;
        }

        /* ── Capture mode (keybindings) ── */
        if (st.mode == Mode::Capture) {
            if (event == Event::Escape) { st.mode = Mode::Normal; return true; }
            if (event == Event::Return) { st.mode = Mode::Normal; return true; }
            if (event == Event::Backspace) {
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
                for (auto &kv : st.kb_map) {
                    auto it = std::find(kv.second.begin(), kv.second.end(), k);
                    if (it != kv.second.end() &&
                        std::addressof(kv.second) != std::addressof(keys))
                        kv.second.erase(it);
                }
                auto it = std::find(keys.begin(), keys.end(), k);
                if (it == keys.end()) {
                    keys.push_back(k);
                    st.notice = "已绑定 " + k + " (再按一次取消)";
                } else {
                    keys.erase(it);
                    st.notice = "已取消 " + k;
                }
                st.dirty_kb = true;
            }
            return true;
        }

        /* ── Normal mode ── */
        std::string k = key_of();

        if (k == "q" || k == "ctrl+c" || k == "escape") {
            if (st.dirty_kb) write_keybindings_yaml(kb_path(), kb_entries(st));
            if (st.dirty_theme) write_theme_yaml(themes_dir() + "/" + st.theme_name + ".yaml",
                                                 st.theme_name, st.theme);
            if (cfg) {
                config_set_int(cfg, "audio.volume", vol);
                config_set_int(cfg, "playback.loop_mode", loop_mode);
                config_set_int(cfg, "playback.seek_step_sec", seek);
                config_set_str(cfg, "ui.theme", cur_theme.c_str());
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
                int n = (int)st.themes.size() + (int)(sizeof(kSlots)/sizeof(kSlots[0]));
                int idx = (st.theme_sel + 1) + dir;
                if (idx < 0) idx = 0;
                if (idx >= n) idx = n - 1;
                /* split: theme list then slot list */
                int tn = (int)st.themes.size();
                if (idx < tn) { st.theme_sel = idx; st.slot_sel = 0; }
                else          { st.slot_sel = idx - tn; }
            }
            return true;
        }
        if (k == "tab") {
            st.section = (st.section + 1) % 3;
            st.mode = Mode::Normal;
            return true;
        }
        if (k == "left" || k == "right") {
            if (st.section == 2) {
                if (k == "left") vol = vol > 0 ? vol - 5 : 0;
                else             vol = vol < 100 ? vol + 5 : 100;
            }
            return true;
        }
        if (k == "l" && st.section == 2) {
            loop_mode = (loop_mode + 1) % 4;
            return true;
        }
        if ((k == "+" || k == "=" || k == "-") && st.section == 2) {
            if (k == "-") seek = seek > 1 ? seek - 1 : 1;
            else          seek = seek < 60 ? seek + 1 : 60;
            return true;
        }
        if (k == "enter" || k == "\r") {
            if (st.section == 0) {
                st.mode = Mode::Capture;
                st.notice.clear();
            } else if (st.section == 1) {
                int tn = (int)st.themes.size();
                if (st.theme_sel < tn) {
                    /* apply theme */
                    cur_theme = st.themes[st.theme_sel];
                    load_theme(st, cur_theme);
                    st.notice = "已应用主题: " + cur_theme + " (退出后生效)";
                } else {
                    st.mode = Mode::ColorEdit;
                    st.hex_buf.clear();
                    st.palette_sel = 0;
                    st.notice.clear();
                }
            }
            return true;
        }
        if (k == "e" || k == "i") {
            if (st.section == 0 || st.section == 1) {
                st.mode = Mode::PathInput;
                st.path_import = (k == "i");
                st.path_for_theme = (st.section == 1);
                st.path_buf.clear();
                st.notice.clear();
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
