/* netune-config — interactive configuration file manager for Netune.
   Browses the config tree (data/themes, data/keybindings, data/layouts,
   data/config.json) and lets the user:
     - apply a theme / edit a keybindings file / tweak playback settings
     - rename, export (copy to a path), delete configuration files
     - import a config file from an arbitrary path into the tree
     - create template files
   UI: top tabs (categories) + file list + bottom action hints; modal
   input/confirm popups; a key-capture editor for keybinding files. */

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
#include <sys/stat.h>

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

enum class Cat { Theme, Keybind, Layout, Main, Playback };

static std::string dir_of(Cat cat);  /* fwd */

/* ── Action metadata ────────────────────────────────── */
struct ActionInfo {
    Action act;
    const char *name;
    const char *desc;
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

enum class Mode { Normal, Input, Confirm, KeyEdit, Capture, ThemeEdit, ColorEdit };

struct CfgFile {
    std::string name;   /* file name (with extension) */
    long size = 0;
};

struct CfgState {
    Mode mode = Mode::Normal;

    Cat cat = Cat::Theme;
    int sel = 0;
    std::vector<CfgFile> files;
    std::vector<CfgFile> kbfiles;   /* cached keybindings list for the editor */

    std::string cur_theme;          /* applied theme name (from config.json) */

    /* input popup */
    std::string input_title;
    std::string input_buf;

    /* confirm popup */
    std::string confirm_msg;
    int confirm_kind = 0;           /* 0 delete, 1 overwrite */

    /* key edit sub-view */
    int kb_sel = 0;
    std::string kb_editing;         /* file name being edited */
    std::vector<std::pair<Action, std::vector<std::string>>> kb_map;

    bool dirty_kb = false;

    /* theme edit sub-view */
    std::string theme_editing;      /* file name being edited */
    Theme theme_edit;
    int slot_sel = 0;
    bool dirty_theme = false;

    /* color edit popup */
    std::string hex_buf;
    int palette_sel = 0;

    /* local music dirs (main config tab) */
    std::vector<std::string> dirs;
    int dir_sel = 0;

    std::string notice;
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
    {"logo",           "网易云Logo", &Theme::logo},
};

/* ── Preset palette (16 colors) ─────────────────────── */
static const char *kPalette[] = {
    "#000000", "#37474f", "#888888", "#b0bec5", "#ffffff",
    "#e53935", "#fb8c00", "#fdd835", "#43a047", "#00acc1",
    "#1e88e5", "#8e24aa", "#e91e63", "#795548", "#9ece6a", "#f7768e",
};
static const int kPaletteN = (int)(sizeof(kPalette)/sizeof(kPalette[0]));

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
    for (auto &slot : kSlots) {
        const ThemeColor &c = t.*(slot.member);
        std::string hex = theme_color_to_hex(c);
        yaml_document_append_mapping_pair(&doc, colors,
            yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)slot.key, (int)strlen(slot.key), YAML_PLAIN_SCALAR_STYLE),
            yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)hex.c_str(), (int)hex.size(), YAML_PLAIN_SCALAR_STYLE));
    }
    yaml_emitter_dump(&em, &doc);
    yaml_document_delete(&doc);
    yaml_emitter_delete(&em);
    fclose(fp);
    return true;
}

static bool load_theme_file(CfgState &st, const std::string &path, const std::string &fname) {
    auto &tm = ThemeManager::instance();
    tm.reset();
    if (!tm.load(path)) return false;
    st.theme_edit = tm.current();
    st.theme_editing = fname;
    st.slot_sel = 0;
    st.dirty_theme = false;
    return true;
}

/* ── Small utilities ────────────────────────────────── */
static std::string dir_of(Cat cat) {
    switch (cat) {
        case Cat::Theme:   return data_root() + "/themes";
        case Cat::Keybind: return data_root() + "/keybindings";
        case Cat::Layout:  return data_root() + "/layouts";
        case Cat::Main:    return data_root();
        default:           return "";
    }
}

static std::string cat_name(Cat cat) {
    switch (cat) {
        case Cat::Theme:   return "主题";
        case Cat::Keybind: return "按键";
        case Cat::Layout:  return "布局";
        case Cat::Main:    return "本地音乐";
        case Cat::Playback:return "播放";
    }
    return "";
}

/* Visible tabs — Layout is hidden until users are ready to edit it */
static const Cat kTabs[] = { Cat::Theme, Cat::Keybind, Cat::Main, Cat::Playback };
static const int kTabCount = (int)(sizeof(kTabs)/sizeof(kTabs[0]));

static int tab_index(Cat cat) {
    for (int i = 0; i < kTabCount; i++)
        if (kTabs[i] == cat) return i;
    return 0;
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

static std::string fmt_size(long n) {
    char buf[32];
    if (n >= 1024) snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
    else           snprintf(buf, sizeof(buf), "%ld B", n);
    return buf;
}

static void refresh_files(CfgState &st) {
    st.files.clear();
    if (st.cat == Cat::Main) {
        /* config.json only */
        std::string p = data_root() + "/config.json";
        struct stat sb;
        if (stat(p.c_str(), &sb) == 0) {
            CfgFile f;
            f.name = "config.json";
            f.size = (long)sb.st_size;
            st.files.push_back(f);
        }
        return;
    }
    if (st.cat == Cat::Playback) return;
    DIR *dp = opendir(dir_of(st.cat).c_str());
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (n.size() > 5 && n.substr(n.size() - 5) == ".yaml") {
            CfgFile f;
            f.name = n;
            std::string p = dir_of(st.cat) + "/" + n;
            struct stat sb;
            if (stat(p.c_str(), &sb) == 0) f.size = (long)sb.st_size;
            st.files.push_back(f);
        }
    }
    closedir(dp);
    std::sort(st.files.begin(), st.files.end(),
              [](const CfgFile &a, const CfgFile &b) { return a.name < b.name; });
    if (st.sel >= (int)st.files.size()) st.sel = (int)st.files.size() - 1;
    if (st.sel < 0) st.sel = 0;
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
        for (auto &key : e.second)
            yaml_document_append_sequence_item(&doc, seq,
                yaml_document_add_scalar(&doc, NULL, (yaml_char_t*)key.c_str(), (int)key.size(), YAML_PLAIN_SCALAR_STYLE));
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

/* default keybindings template (matches app.cpp) */
static std::vector<std::pair<std::string, std::vector<std::string>>> default_kb_entries(void) {
    KeybindingManager km;  /* no load → built-in defaults */
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    for (auto &a : kActions) {
        std::vector<std::string> keys = km.keys_for(a.act);
        if (!keys.empty())
            out.push_back({a.name, std::move(keys)});
    }
    return out;
}

/* default theme template (Tokyo Night, matches app.cpp DEFAULT_THEME_DEFAULT_YAML) */
static const char *kThemeTemplate =
    "name: \"Tokyo Night\"\n"
    "colors:\n"
    "  bg: \"#1a1b26\"\n"
    "  fg: \"#c0caf5\"\n"
    "  accent: \"#7aa2f7\"\n"
    "  accent_bg: \"#33467c\"\n"
    "  muted: \"#565f89\"\n"
    "  border: \"#292e42\"\n"
    "  success: \"#9ece6a\"\n"
    "  warning: \"#e0af68\"\n"
    "  error: \"#f7768e\"\n"
    "  vip: \"#e0af68\"\n"
    "  playlist: \"#7dcfff\"\n"
    "  logo: \"#7dcfff\"\n"
    "  overlay_bg: \"#16161e\"\n";

/* default layout template (matches app.cpp DEFAULT_LAYOUT_YAML) */
static const char *kLayoutTemplate =
    "layout:\n"
    "  type: \"vertical\"\n"
    "  children:\n"
    "    - component: \"top_bar\"\n"
    "      height: 1\n"
    "    - type: \"horizontal\"\n"
    "      flex: 1\n"
    "      children:\n"
    "        - component: \"group_list\"\n"
    "          width: 20\n"
    "        - component: \"song_list\"\n"
    "          flex: 1\n"
    "    - component: \"status_bar\"\n"
    "      height: 2\n";

static bool write_text_file(const std::string &path, const std::string &content) {
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) return false;
    fwrite(content.data(), 1, content.size(), fp);
    fclose(fp);
    return true;
}

/* ── Action helpers ─────────────────────────────────── */
static bool apply_theme(CfgState &st, Config *cfg, const std::string &name) {
    st.cur_theme = name;
    bool ok = true;
    if (cfg) {
        config_set_str(cfg, "ui.theme", name.c_str());
        ok = config_save(cfg);
    }
    return ok;
}

static bool load_kb_file(CfgState &st, const std::string &path) {
    st.kb_map.clear();
    KeybindingManager km;
    km.load(path);
    for (auto &a : kActions)
        st.kb_map.push_back({a.act, km.keys_for(a.act)});
    st.kb_sel = 0;
    st.dirty_kb = false;
    return true;
}

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

    Config *cfg = config_load((data_root() + "/config.json").c_str());
    st.cur_theme = cfg ? config_get_str(cfg, "ui.theme", "default") : "default";
    int cur_vol  = cfg ? config_get_int(cfg, "audio.volume", 80) : 80;
    int cur_loop = cfg ? config_get_int(cfg, "playback.loop_mode", 0) : 0;
    int cur_seek = cfg ? config_get_int(cfg, "playback.seek_step_sec", 5) : 5;
    if (cur_seek < 1) cur_seek = 1;
    if (cur_seek > 60) cur_seek = 60;
    int vol = cur_vol, loop_mode = cur_loop, seek = cur_seek;

    int ndirs = cfg ? config_get_array_size(cfg, "music_sources.local.dirs") : 0;
    for (int i = 0; i < ndirs; i++) {
        char key[64];
        snprintf(key, sizeof(key), "music_sources.local.dirs[%d]", i);
        const char *d = cfg ? config_get_str(cfg, key, NULL) : NULL;
        if (d) st.dirs.push_back(d);
    }

    refresh_files(st);

    /* ── Modal popups (input / confirm / capture) ────── */
    auto render_popup = [&]() -> Element {
        auto &th = st;
        if (th.mode == Mode::Input) {
            Elements body;
            body.push_back(text("  " + th.input_title) | bold);
            body.push_back(separator());
            body.push_back(text("  " + th.input_buf + "\u258C"));
            body.push_back(separator());
            body.push_back(text("  Enter 确认   ESC 取消") | dim);
            auto box = vbox(std::move(body)) | borderRounded
                       | bgcolor(Color::RGB(26,27,38))
                       | color(Color::RGB(122,162,247));
            return vbox({filler(), hbox({filler(), box})});
        }
        if (th.mode == Mode::Confirm) {
            Elements body;
            body.push_back(text("  " + th.confirm_msg) | bold | color(Color::RGB(247,118,142)));
            body.push_back(separator());
            body.push_back(text("  y 确认   n/ESC 取消") | dim);
            auto box = vbox(std::move(body)) | borderRounded
                       | bgcolor(Color::RGB(26,27,38))
                       | color(Color::RGB(122,162,247));
            return vbox({filler(), hbox({filler(), box})});
        }
        if (th.mode == Mode::Capture) {
            Elements body;
            body.push_back(text("  编辑按键: 按新键=绑定(支持 ctrl/alt), 已有键=取消") | bold);
            body.push_back(separator());
            body.push_back(text("  当前: " + key_list_str(th.kb_map[th.kb_sel].second)));
            body.push_back(text("  Backspace 删除最后一个  Enter 完成  ESC 取消") | dim);
            auto box = vbox(std::move(body)) | borderRounded
                       | bgcolor(Color::RGB(26,27,38))
                       | color(Color::RGB(122,162,247));
            return vbox({filler(), hbox({filler(), box})});
        }
        if (th.mode == Mode::ColorEdit) {
            const ColorSlot &slot = kSlots[th.slot_sel];
            ThemeColor &c = th.theme_edit.*(slot.member);
            Elements body;
            body.push_back(text(std::string("  编辑颜色 [") + slot.name + "]  当前 " +
                                theme_color_to_hex(c)) | bold);
            body.push_back(separator());
            Element swatch = c.has_color
                ? text("  ") | bgcolor(Color::RGB(c.r, c.g, c.b))
                : text(" · ");
            body.push_back(hbox({swatch, text("  输入 hex (如 #1a1b26): "),
                                 text(th.hex_buf + "\u258C")}));
            Elements pal;
            for (int i = 0; i < kPaletteN; i++) {
                ThemeColor pc = theme_color_from_hex(kPalette[i]);
                auto p = text("  ");
                if (i == th.palette_sel)
                    p = p | bold | inverted;
                pal.push_back(p | bgcolor(Color::RGB(pc.r, pc.g, pc.b)));
            }
            body.push_back(hbox(std::move(pal)));
            body.push_back(text("  ←/→ 选色板  x 无色  Enter 应用  ESC 取消") | dim);
            auto box = vbox(std::move(body)) | borderRounded
                       | bgcolor(Color::RGB(26,27,38))
                       | color(Color::RGB(122,162,247));
            return vbox({filler(), hbox({filler(), box})});
        }
        return text("");
    };


    /* ── Renderer ───────────────────────────────────── */
    auto render_frame = [&]() -> Element {
        auto &th = st;
        Elements els;

        /* top bar: tabs */
        Elements tabs;
        for (int i = 0; i < kTabCount; i++) {
            Cat c = kTabs[i];
            bool sel = (th.cat == c);
            auto t = text(" " + cat_name(c) + " ");
            if (sel) tabs.push_back(t | bold | inverted);
            else     tabs.push_back(t);
            if (i < kTabCount - 1) tabs.push_back(text("│") | dim);
        }
        els.push_back(hbox(std::move(tabs)) | border);

        /* body */
        if (th.cat == Cat::Playback) {
            const char *loops[] = {"顺序播放", "单曲循环", "列表循环", "随机播放"};
            Elements body;
            body.push_back(text(std::string("  音量:  ") + std::to_string(vol) + "   [<- / ->]"));
            body.push_back(text(std::string("  循环:  ") + loops[loop_mode % 4] + "   [l 切换]"));
            body.push_back(text(std::string("  快进步长: ") + std::to_string(seek) + " 秒  [+ / -]"));
            body.push_back(text(""));
            body.push_back(text("  修改立即保存到 config.json") | dim);
            els.push_back(vbox(std::move(body)) | flex | border);
        } else if (th.mode == Mode::KeyEdit || th.mode == Mode::Capture) {
            /* key editor sub-view */
            Elements body;
            body.push_back(text("  编辑按键: " + th.kb_editing + "   (ESC 返回)") | bold);
            body.push_back(separator());
            for (size_t i = 0; i < sizeof(kActions)/sizeof(kActions[0]); i++) {
                bool sel = ((int)i == th.kb_sel);
                std::string line = "  " + std::string(kActions[i].desc) + " = " +
                                   key_list_str(th.kb_map[i].second);
                if (sel)
                    body.push_back(hbox({text("> "), text(line) | bold}) | inverted);
                else
                    body.push_back(text(line));
            }
            body.push_back(text(""));
            body.push_back(text("  Enter: 绑定  Backspace: 删除最后绑定  ESC: 返回") | dim);
            els.push_back(vbox(std::move(body)) | flex | border);
        } else if (th.mode == Mode::ThemeEdit || th.mode == Mode::ColorEdit) {
            /* theme color-slot editor sub-view */
            Elements body;
            body.push_back(text("  编辑主题: " + th.theme_editing + "   (ESC 保存返回)") | bold);
            body.push_back(separator());
            for (size_t i = 0; i < sizeof(kSlots)/sizeof(kSlots[0]); i++) {
                const ColorSlot &slot = kSlots[i];
                const ThemeColor &c = th.theme_edit.*(slot.member);
                bool sel = ((int)i == th.slot_sel);
                Element swatch = c.has_color
                ? text("  ") | bgcolor(Color::RGB(c.r, c.g, c.b))
                : text(" · ");
                std::string line = "  " + std::string(slot.name) + " = " +
                                   theme_color_to_hex(c);
                Element row = hbox({swatch, text(line)});
                if (sel)
                    body.push_back(hbox({text("> "), row | bold}) | inverted);
                else
                    body.push_back(hbox({text("  "), row}));
            }
            body.push_back(text(""));
            body.push_back(text("  Enter: 编辑颜色(hex/色板)  ↑/↓: 选择  ESC: 保存返回") | dim);
            els.push_back(vbox(std::move(body)) | flex | border);
        } else if (th.cat == Cat::Main) {
            Elements body;
            body.push_back(text("  本地音乐目录 (music_sources.local.dirs)") | bold);
            body.push_back(separator());
            if (th.dirs.empty()) {
                body.push_back(text("  (未配置 — 本地音乐不可用)") | dim);
            } else {
                for (size_t i = 0; i < th.dirs.size(); i++) {
                    bool sel = ((int)i == th.dir_sel);
                    std::string line = "  " + std::to_string(i + 1) + ". " + th.dirs[i];
                    if (sel)
                        body.push_back(hbox({text("> "), text(line) | bold}) | inverted);
                    else
                        body.push_back(text(line));
                }
            }
            body.push_back(text(""));
            body.push_back(text("  a 添加目录   d 删除选中   (修改立即保存, 重启 netune 生效)") | dim);
            body.push_back(text("  目录列表写入 config.json 的 music_sources.local.dirs") | dim);
            els.push_back(vbox(std::move(body)) | flex | border);
        } else {
            /* file list */
            Elements body;
            if (th.files.empty()) {
                body.push_back(text("  (无配置文件)") | dim);
            }
            for (size_t i = 0; i < th.files.size(); i++) {
                bool sel = ((int)i == th.sel);
                const CfgFile &f = th.files[i];
                std::string name = f.name;
                if (th.cat == Cat::Theme) {
                    std::string base = f.name;
                    if (base.size() > 5) base = base.substr(0, base.size() - 5);
                    if (base == th.cur_theme) name += "  (当前)";
                }
                std::string line = std::string("  ") + name +
                                   std::string(name.size() < 26 ? 26 - name.size() : 1, ' ') +
                                   fmt_size(f.size);
                if (sel)
                    body.push_back(hbox({text("> "), text(line) | bold}) | inverted);
                else
                    body.push_back(text(line));
            }
            els.push_back(vbox(std::move(body)) | flex | border);
        }

        /* notice */
        if (!th.notice.empty())
            els.push_back(text("  " + th.notice) | color(Color::Green));

        /* action hints */
        std::string hints;
        switch (th.cat) {
            case Cat::Theme:   hints = "Enter 选用   x 编辑颜色   r 重命名   e 导出   d 删除   i 导入   n 新建模板"; break;
            case Cat::Keybind: hints = "Enter 选用(复制为default)   x 编辑按键   r 重命名   e 导出   d 删除   i 导入   n 新建模板"; break;
            case Cat::Layout:  hints = "Enter 选用   r 重命名   e 导出   d 删除   i 导入   n 新建模板"; break;
            case Cat::Main:    hints = "a 添加音乐目录   d 删除选中   ↑/↓ 选择"; break;
            case Cat::Playback:hints = "←/→ 音量   l 循环   + / - 快进步长"; break;
        }
        els.push_back(text("  " + hints) | dim);
        els.push_back(text("  ←/→ 或 1-4 切换分类   q 退出") | dim);

        return dbox({
            vbox(std::move(els)) | flex | border | bgcolor(Color::RGB(26,27,38)),
            render_popup(),
        });
    };

    auto main = Renderer(render_frame);
    main |= CatchEvent([&](Event event) -> bool {
        if (event.is_mouse()) return true;

        auto key_of = [&]() -> std::string {
            if (event.is_character()) return event.character();
            return event_to_key_name(event);
        };

        /* ── Color edit popup ── */
        if (st.mode == Mode::ColorEdit) {
            std::string k = key_of();
            if (event == Event::Escape) { st.mode = Mode::ThemeEdit; return true; }
            if (event == Event::ArrowLeft || k == "left") {
                st.palette_sel = (st.palette_sel + kPaletteN - 1) % kPaletteN;
                return true;
            }
            if (event == Event::ArrowRight || k == "right") {
                st.palette_sel = (st.palette_sel + 1) % kPaletteN;
                return true;
            }
            if (k == "x") {
                st.theme_edit.*(kSlots[st.slot_sel].member) = ThemeColor{};
                st.dirty_theme = true;
                st.notice = std::string("已设置 ") + kSlots[st.slot_sel].name + " = 无色";
                st.hex_buf.clear();
                st.mode = Mode::ThemeEdit;
                return true;
            }
            if (event == Event::Backspace) {
                if (!st.hex_buf.empty()) st.hex_buf.pop_back();
                return true;
            }
            if (event == Event::Return || k == "\r") {
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
                    st.theme_edit.*(kSlots[st.slot_sel].member) = nc;
                    st.dirty_theme = true;
                    st.notice = std::string("已设置 ") + kSlots[st.slot_sel].name +
                                " = " + theme_color_to_hex(nc);
                } else {
                    st.notice = "无效的 hex 颜色";
                }
                st.hex_buf.clear();
                st.mode = Mode::ThemeEdit;
                return true;
            }
            if (k.size() == 1 && st.hex_buf.size() < 9) {
                char c = k[0];
                if (c == '#' || (c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                    st.hex_buf += c;
            }
            return true;
        }

        /* ── Theme edit sub-view ── */
        if (st.mode == Mode::ThemeEdit) {
            std::string k = key_of();
            if (event == Event::Escape) {
                if (st.dirty_theme) {
                    bool ok = write_theme_yaml(dir_of(Cat::Theme) + "/" + st.theme_editing,
                                               st.theme_editing, st.theme_edit);
                    st.notice = ok ? "已保存 " + st.theme_editing : "保存失败";
                }
                st.mode = Mode::Normal;
                refresh_files(st);
                return true;
            }
            if (k == "up" || k == "down" || k == "j" || k == "k") {
                int dir = (k == "up" || k == "k") ? -1 : 1;
                int n = (int)(sizeof(kSlots)/sizeof(kSlots[0]));
                st.slot_sel = (st.slot_sel + dir + n) % n;
                return true;
            }
            if (k == "enter" || k == "\r") {
                st.hex_buf.clear();
                st.palette_sel = 0;
                st.mode = Mode::ColorEdit;
                st.notice.clear();
                return true;
            }
            return true;
        }

        /* ── Capture (keybinding) ── */
        if (st.mode == Mode::Capture) {
            if (event == Event::Escape || event == Event::Return) { st.mode = Mode::KeyEdit; return true; }
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
                if (it == keys.end()) { keys.push_back(k); st.notice = "已绑定 " + k; }
                else                  { keys.erase(it);     st.notice = "已取消 " + k; }
                st.dirty_kb = true;
            }
            return true;
        }

        /* ── Confirm popup ── */
        if (st.mode == Mode::Confirm) {
            std::string k = key_of();
            if (k == "y" || k == "Y") {
                if (st.confirm_kind == 2) {
                    /* delete a local music dir */
                    if (cfg && st.dir_sel >= 0 &&
                        st.dir_sel < (int)st.dirs.size()) {
                        if (config_array_remove(cfg, "music_sources.local.dirs",
                                                st.dir_sel) && config_save(cfg)) {
                            st.notice = "已删除目录: " + st.dirs[st.dir_sel];
                            st.dirs.erase(st.dirs.begin() + st.dir_sel);
                            if (st.dir_sel >= (int)st.dirs.size())
                                st.dir_sel = (int)st.dirs.size() - 1;
                            if (st.dir_sel < 0) st.dir_sel = 0;
                        } else {
                            st.notice = "删除失败";
                        }
                    }
                } else {
                    std::string target = dir_of(st.cat) + "/" + st.files[st.sel].name;
                    if (st.confirm_kind == 0) {
                        if (remove(target.c_str()) == 0) {
                            st.notice = "已删除 " + st.files[st.sel].name;
                            refresh_files(st);
                        } else {
                            st.notice = "删除失败";
                        }
                    } else {
                        /* overwrite existing (import/rename/export collision) */
                        if (copy_file(st.input_buf, target)) {
                            st.notice = "已覆盖 " + st.files[st.sel].name;
                            refresh_files(st);
                        } else {
                            st.notice = "写入失败";
                        }
                    }
                }
                st.mode = Mode::Normal;
                return true;
            }
            if (k == "n" || k == "N" || event == Event::Escape) {
                st.notice = "已取消";
                st.mode = Mode::Normal;
                return true;
            }
            return true;
        }

        /* ── Input popup ── */
        if (st.mode == Mode::Input) {
            std::string k = key_of();
            if (event == Event::Escape) { st.mode = Mode::Normal; return true; }
            if (event == Event::Backspace) {
                if (!st.input_buf.empty()) st.input_buf.pop_back();
                return true;
            }
            if (event == Event::Return || k == "\r") {
                std::string name = st.input_buf;
                st.mode = Mode::Normal;
                st.notice.clear();
                if (name.empty()) return true;

                if (st.input_title == "添加音乐目录") {
                    if (cfg && config_array_push_str(cfg,
                            "music_sources.local.dirs", name.c_str()) &&
                        config_save(cfg)) {
                        st.notice = "已添加目录: " + name;
                        st.dirs.push_back(name);
                        st.dir_sel = (int)st.dirs.size() - 1;
                    } else {
                        st.notice = "添加失败";
                    }
                    return true;
                }

                /* switch on the current popup intent (stored in input_title tag) */
                std::string action = st.input_title;
                std::string dir = dir_of(st.cat);
                std::string src = dir + "/" + st.files[st.sel].name;
                if (action == "重命名") {
                    if (name.find('.') == std::string::npos) name += ".yaml";
                    std::string dst = dir + "/" + name;
                    if (src == dst) return true;
                    if (rename(src.c_str(), dst.c_str()) == 0) {
                        st.notice = "已重命名为 " + name;
                        refresh_files(st);
                    } else {
                        st.notice = "重命名失败";
                    }
                } else if (action == "导出到路径") {
                    if (copy_file(src, name)) st.notice = "已导出: " + name;
                    else st.notice = "导出失败";
                } else if (action == "导入文件路径") {
                    /* validate + copy into the tree (overwrite without confirm) */
                    bool ok = false;
                    if (st.cat == Cat::Theme)   ok = yaml_validate(name, "colors");
                    if (st.cat == Cat::Keybind) ok = yaml_validate(name, "keybindings");
                    if (st.cat == Cat::Layout)  ok = yaml_validate(name, "layout");
                    if (st.cat == Cat::Main)    ok = true;
                    if (!ok) {
                        st.notice = "不是有效的配置文件";
                    } else {
                        std::string base = basename_of(name);
                        if (st.cat != Cat::Main && base.find('.') == std::string::npos)
                            base += ".yaml";
                        if (st.cat == Cat::Main) base = "config.json";
                        if (copy_file(name, dir + "/" + base)) {
                            st.notice = "已导入: " + base;
                            refresh_files(st);
                        } else {
                            st.notice = "导入失败";
                        }
                    }
                } else if (action == "新建模板") {
                    if (name.find('.') == std::string::npos) name += ".yaml";
                    std::string dst = dir + "/" + name;
                    bool ok = false;
                    if (st.cat == Cat::Theme)   ok = write_text_file(dst, kThemeTemplate);
                    if (st.cat == Cat::Keybind) ok = write_keybindings_yaml(dst, default_kb_entries());
                    if (st.cat == Cat::Layout)  ok = write_text_file(dst, kLayoutTemplate);
                    if (ok) {
                        st.notice = "已创建模板: " + name;
                        refresh_files(st);
                    } else {
                        st.notice = "创建失败";
                    }
                }
                return true;
            }
            if (event.is_character() && st.input_buf.size() < 400) {
                const std::string &c = event.character();
                if (!c.empty()) st.input_buf += c;
            }
            return true;
        }

        /* ── Key edit sub-view ── */
        if (st.mode == Mode::KeyEdit) {
            std::string k = key_of();
            if (event == Event::Escape) {
                /* save & return */
                if (st.dirty_kb)
                    write_keybindings_yaml(dir_of(Cat::Keybind) + "/" + st.kb_editing, kb_entries(st));
                st.mode = Mode::Normal;
                refresh_files(st);
                return true;
            }
            if (k == "up" || k == "down" || k == "j" || k == "k") {
                int dir = (k == "up" || k == "k") ? -1 : 1;
                int n = (int)(sizeof(kActions)/sizeof(kActions[0]));
                st.kb_sel = (st.kb_sel + dir + n) % n;
                return true;
            }
            if (k == "enter" || k == "\r") {
                st.mode = Mode::Capture;
                st.notice.clear();
                return true;
            }
            return true;
        }

        /* ── Normal mode ── */
        std::string k = key_of();

        if (k == "q" || k == "ctrl+c") {
            if (cfg) {
                config_set_int(cfg, "audio.volume", vol);
                config_set_int(cfg, "playback.loop_mode", loop_mode);
                config_set_int(cfg, "playback.seek_step_sec", seek);
                config_free(cfg);
            }
            screen.ExitLoopClosure()();
            return true;
        }
        if (k.size() == 1 && k[0] >= '1' && k[0] <= '0' + kTabCount) {
            st.cat = kTabs[k[0] - '1'];
            st.mode = Mode::Normal;
            refresh_files(st);
            return true;
        }
        if (k == "left" || k == "right" || k == "tab") {
            if (k == "tab") {
                st.cat = kTabs[(tab_index(st.cat) + 1) % kTabCount];
                refresh_files(st);
                return true;
            }
            if (st.cat == Cat::Playback) {
                if (k == "left") vol = vol > 0 ? vol - 5 : 0;
                else             vol = vol < 100 ? vol + 5 : 100;
                if (cfg) {
                    config_set_int(cfg, "audio.volume", vol);
                    config_save(cfg);
                }
            }
            return true;
        }
        if (k == "up" || k == "down" || k == "j" || k == "k") {
            int dir = (k == "up" || k == "k") ? -1 : 1;
            if (st.cat == Cat::Main) {
                int n = (int)st.dirs.size();
                if (n > 0) st.dir_sel = (st.dir_sel + dir + n) % n;
            } else {
                int n = (int)st.files.size();
                if (n > 0) st.sel = (st.sel + dir + n) % n;
            }
            return true;
        }
        if (k == "a" && st.cat == Cat::Main) {
            st.input_title = "添加音乐目录";
            st.input_buf = "";
            st.mode = Mode::Input;
            st.notice.clear();
            return true;
        }
        if (k == "d" && st.cat == Cat::Main && !st.dirs.empty()) {
            st.confirm_kind = 2;
            st.confirm_msg = "删除音乐目录: " + st.dirs[st.dir_sel] + " ? (y/n)";
            st.mode = Mode::Confirm;
            st.notice.clear();
            return true;
        }
        if (k == "l" && st.cat == Cat::Playback) {
            loop_mode = (loop_mode + 1) % 4;
            if (cfg) {
                config_set_int(cfg, "playback.loop_mode", loop_mode);
                config_save(cfg);
            }
            return true;
        }
        if ((k == "+" || k == "=" || k == "-") && st.cat == Cat::Playback) {
            if (k == "-") seek = seek > 1 ? seek - 1 : 1;
            else          seek = seek < 60 ? seek + 1 : 60;
            if (cfg) {
                config_set_int(cfg, "playback.seek_step_sec", seek);
                config_save(cfg);
            }
            return true;
        }
        if (k == "enter" || k == "\r") {
            if (st.cat == Cat::Theme && !st.files.empty()) {
                std::string base = st.files[st.sel].name;
                if (base.size() > 5) base = base.substr(0, base.size() - 5);
                st.notice = apply_theme(st, cfg, base) ? "已加载配置" : "加载失败";
            } else if (st.cat == Cat::Keybind && !st.files.empty()) {
                std::string sel = st.files[st.sel].name;
                if (sel == "default.yaml") {
                    st.notice = "已加载配置";
                } else if (copy_file(dir_of(Cat::Keybind) + "/" + sel,
                                     dir_of(Cat::Keybind) + "/default.yaml")) {
                    st.notice = "已加载配置";
                } else {
                    st.notice = "加载失败";
                }
            } else if (st.cat == Cat::Layout && !st.files.empty()) {
                std::string base = st.files[st.sel].name;
                if (base.size() > 5) base = base.substr(0, base.size() - 5);
                bool ok = false;
                if (cfg) {
                    config_set_str(cfg, "ui.layout", base.c_str());
                    ok = config_save(cfg);
                }
                st.notice = ok ? "已加载配置" : "加载失败";
            }
            return true;
        }
        if (k == "x" && (st.cat == Cat::Theme || st.cat == Cat::Keybind) &&
            !st.files.empty()) {
            /* x = edit the selected config file */
            if (st.cat == Cat::Theme) {
                load_theme_file(st, dir_of(Cat::Theme) + "/" + st.files[st.sel].name,
                                st.files[st.sel].name);
                st.mode = Mode::ThemeEdit;
            } else {
                st.kb_editing = st.files[st.sel].name;
                load_kb_file(st, dir_of(Cat::Keybind) + "/" + st.kb_editing);
                st.mode = Mode::KeyEdit;
            }
            st.notice.clear();
            return true;
        }
        if ((k == "r" || k == "e" || k == "i" || k == "n" || k == "d") &&
            st.cat != Cat::Main && st.cat != Cat::Playback) {
            if (st.files.empty() && (k == "r" || k == "e" || k == "d")) return true;
            st.notice.clear();
            if (k == "r") {
                st.input_title = "重命名";
                st.input_buf = st.files.empty() ? "" : st.files[st.sel].name;
                st.mode = Mode::Input;
            } else if (k == "e") {
                st.input_title = "导出到路径";
                st.input_buf = "";
                st.mode = Mode::Input;
            } else if (k == "i") {
                st.input_title = "导入文件路径";
                st.input_buf = "";
                st.mode = Mode::Input;
            } else if (k == "n") {
                st.input_title = "新建模板";
                st.input_buf = "";
                st.mode = Mode::Input;
            } else if (k == "d") {
                st.confirm_kind = 0;
                st.confirm_msg = "确认删除 " + st.files[st.sel].name + " ?";
                st.mode = Mode::Confirm;
            }
            return true;
        }
        if (k == "escape") {
            screen.ExitLoopClosure()();
            return true;
        }
        return true;
    });

    ftxui::Loop loop(&screen, main);
    loop.Run();
    screen.ExitLoopClosure()();
    return 0;
}
