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
#include <math.h>
#include <dirent.h>          /* compat shim on Windows (netune target adds src/compat) */
#include <sys/stat.h>

#include "compat/utf8.h"     /* fopen_utf8/stat_utf8/...: ANSI-codepage-safe file I/O on MSVC */

#include "infra/config.h"
#include "infra/config_paths.h"
#include "netune_config.h"
#include "core/audio_cache.h"
#include "ui/keybindings.h"
#include "ui/theme.h"
#include "ui/components/theme_util.h"
#include <yaml.h>

using namespace ftxui;

/* ── Path helpers ─────────────────────────────────────
   Thin adapter over the shared infra/config_paths module (the same
   implementation app.cpp uses).  The old local copy had drifted from
   app.cpp — it was missing the Windows APPDATA branch, so on Windows
   this tool would look for config.json under /tmp/.config/... */
static std::string data_root(void) {
    return std::string(netune_data_root());
}

enum class Cat { Theme, Keybind, Layout, Main, Playback, Cache };

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
    /* 2D gradient picker: 纵=色相(可滚动) 横=饱和度 底部=亮度 */
    int pick_hue = 0;   /* 0..HUE_ROWS-1 */
    int pick_sat = 0;   /* 0..SAT_COLS-1 */
    int pick_val = 200; /* 0..255 (亮度, 底部固定) */
    int hue_top = 0;    /* 视口顶部色相行 */
    int pick_focus = 0; /* Tab 焦点: 0=二维表 1=亮度条 2=hex */

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
    {"border",         "边框",      &Theme::border},
    {"success",        "成功",      &Theme::success},
    {"warning",        "警告",      &Theme::warning},
    {"error",          "错误",      &Theme::error},
    {"overlay_bg",     "弹窗背景",  &Theme::overlay_bg},
    {"popup_border",   "小窗边框",  &Theme::popup_border},
    {"progress_track", "进度条轨道", &Theme::progress_track},
    {"spectrum",       "频谱",      &Theme::spectrum},
    {"vip",            "VIP 标记",  &Theme::vip},
    {"svip",           "SVIP 标记", &Theme::svip},
    {"playlist",       "歌单标记",  &Theme::playlist},
    {"logo",           "网易云Logo", &Theme::logo},
    {"artist",         "艺人名字",  &Theme::artist},
};

/* ── 2D gradient picker (hue × saturation) ─────────────
   A full-screen color picker: the grid maps hue vertically (scrollable,
   0°..360°) and saturation horizontally (0%..100%); brightness is NOT in
   the grid — it sits on a fixed bar at the bottom and never scrolls. */
#define SAT_COLS 20   /* 饱和度列数 (20%..100%) */
#define HUE_ROWS 72   /* 色相行数 (每格 5°, 0°..355°) — 可滚动 */
#define HUE_VIEW 12   /* 一屏可见色相行数 */

static void hsv_to_rgb(double h, double s, double v, int *r, int *g, int *b) {
    if (s <= 0) {
        int l = (int)(v * 255 + 0.5);
        *r = *g = *b = l;
        return;
    }
    h = fmod(h, 360.0);
    if (h < 0) h += 360.0;
    int sector = (int)(h / 60.0) % 6;
    double f = h / 60.0 - sector;
    double p = v * (1 - s);
    double q = v * (1 - s * f);
    double t = v * (1 - s * (1 - f));
    double rr, gg, bb;
    switch (sector) {
        case 0: rr = v; gg = t; bb = p; break;
        case 1: rr = q; gg = v; bb = p; break;
        case 2: rr = p; gg = v; bb = t; break;
        case 3: rr = p; gg = q; bb = v; break;
        case 4: rr = t; gg = p; bb = v; break;
        default: rr = v; gg = p; bb = q; break;
    }
    *r = (int)(rr * 255 + 0.5);
    *g = (int)(gg * 255 + 0.5);
    *b = (int)(bb * 255 + 0.5);
}

static void rgb_to_hsv(int r, int g, int b, double *h, double *s, double *v) {
    double rr = r / 255.0, gg = g / 255.0, bb = b / 255.0;
    double mx = std::max({rr, gg, bb});
    double mn = std::min({rr, gg, bb});
    double d = mx - mn;
    *v = mx;
    *s = mx == 0 ? 0 : d / mx;
    double hh = 0;
    if (d != 0) {
        if (mx == rr)      hh = 60 * fmod((gg - bb) / d, 6.0);
        else if (mx == gg) hh = 60 * ((bb - rr) / d + 2);
        else               hh = 60 * ((rr - gg) / d + 4);
    }
    if (hh < 0) hh += 360;
    *h = hh;
}

/* Color of a cell: hue row index + saturation column index + brightness.
   hue descends top→bottom (top = 355°, bottom = 0°); sat descends
   left→right (100% → 20%). */
static ThemeColor pick_cell_color(int hue_idx, int sat_idx, int val) {
    double hue = (HUE_ROWS - 1 - hue_idx + 0.5) * 360.0 / HUE_ROWS;
    double sat = 1.0 - 0.80 * sat_idx / (SAT_COLS - 1);
    double v = val / 255.0;
    int r, g, b;
    hsv_to_rgb(hue, sat, v, &r, &g, &b);
    ThemeColor c;
    c.r = (uint8_t)r; c.g = (uint8_t)g; c.b = (uint8_t)b;
    c.has_color = true;
    return c;
}

static bool write_theme_yaml(const std::string &path, const std::string &name, const Theme &t) {
    FILE *fp = fopen_utf8(path.c_str(), "wb");
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
        case Cat::Cache:   return "缓存";
    }
    return "";
}

/* Visible tabs — Layout is hidden until users are ready to edit it */
static const Cat kTabs[] = { Cat::Theme, Cat::Keybind, Cat::Main, Cat::Playback, Cat::Cache };
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
    FILE *in = fopen_utf8(src.c_str(), "rb");
    if (!in) return false;
    FILE *out = fopen_utf8(dst.c_str(), "wb");
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
    FILE *fp = fopen_utf8(path.c_str(), "rb");
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

/* Human-readable byte count: 800 KB / 350 MB / 1.2 GB / ... */
static std::string fmt_capacity(long long bytes) {
    char buf[32];
    double v;
    const char *u;
    if (bytes >= 1024LL * 1024 * 1024) { v = bytes / (1024.0 * 1024 * 1024); u = "GB"; }
    else if (bytes >= 1024 * 1024)      { v = bytes / (1024.0 * 1024);       u = "MB"; }
    else if (bytes >= 1024)             { v = bytes / 1024.0;                u = "KB"; }
    else                                { v = (double)bytes;                 u = "B"; }
    snprintf(buf, sizeof buf, "%.1f %s", v, u);
    return buf;
}

static void refresh_files(CfgState &st) {
    st.files.clear();
    if (st.cat == Cat::Main) {
        /* config.json only */
        std::string p = data_root() + "/config.json";
        struct stat sb;
        if (stat_utf8(p.c_str(), &sb) == 0) {
            CfgFile f;
            f.name = "config.json";
            f.size = (long)sb.st_size;
            st.files.push_back(f);
        }
        return;
    }
    if (st.cat == Cat::Playback) return;
    if (st.cat == Cat::Cache) return;
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
            if (stat_utf8(p.c_str(), &sb) == 0) f.size = (long)sb.st_size;
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
    FILE *fp = fopen_utf8(path.c_str(), "wb");
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
    "  border: \"#292e42\"\n"
    "  success: \"#9ece6a\"\n"
    "  warning: \"#e0af68\"\n"
    "  error: \"#f7768e\"\n"
    "  vip: \"#e0af68\"\n"
    "  svip: \"#bb9af7\"\n"
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
    FILE *fp = fopen_utf8(path.c_str(), "wb");
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

/* Entry point — invoked from main.cpp when launched as `netune --config`.
 * (Was `int main()` of the old standalone netune-config executable.) */
int run_config(void) {
    auto screen = ScreenInteractive::Fullscreen();
    CfgState st;

    Config *cfg = config_load((data_root() + "/config.json").c_str());
    st.cur_theme = cfg ? config_get_str(cfg, "ui.theme", "default") : "default";
    /* Load the applied theme so the manager's own chrome (background, text,
       popups) uses the same theme system as the main app, not hardcoded RGB. */
    ThemeManager::instance().load(
        ThemeManager::resolve_path(st.cur_theme));
    int cur_vol  = cfg ? config_get_int(cfg, "audio.volume", 80) : 80;
    int cur_loop = cfg ? config_get_int(cfg, "playback.loop_mode", 0) : 0;
    int cur_seek = cfg ? config_get_int(cfg, "playback.seek_step_sec", 5) : 5;
    if (cur_seek < 1) cur_seek = 1;
    if (cur_seek > 60) cur_seek = 60;
    int vol = cur_vol, loop_mode = cur_loop, seek = cur_seek;
    std::string quality = cfg ? config_get_str(cfg, "netease.quality", "exhigh") : "exhigh";

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
        Elements body;
        if (th.mode == Mode::Input) {
            body.push_back(text(" " + th.input_title) | bold);
            body.push_back(separator());
            body.push_back(text(" " + th.input_buf + "\u258C"));
            body.push_back(separator());
            body.push_back(text(" Enter 确认   ESC 取消") | dim);
        } else if (th.mode == Mode::Confirm) {
            body.push_back(text(" " + th.confirm_msg) | bold);
            body.push_back(separator());
            body.push_back(text(" y 确认   n/ESC 取消") | dim);
        } else if (th.mode == Mode::Capture) {
            body.push_back(text(" 编辑按键: 按新键=绑定(支持 ctrl/alt), 已有键=取消") | bold);
            body.push_back(separator());
            body.push_back(text(" 当前: " + key_list_str(th.kb_map[th.kb_sel].second)));
            body.push_back(separator());
            body.push_back(text(" Backspace 删除最后一个  Enter 完成  ESC 取消") | dim);
        }
        if (body.empty()) return text("");
        return vbox({filler(), hbox({filler(), vbox(std::move(body)) | border})});
    };


    /* ── Renderer ───────────────────────────────────── */
    auto render_frame = [&]() -> Element {
        auto &th = st;
        Elements els;

        /* top bar: tabs (selected inverted, others dimmed; no inner border) */
        Elements tabs;
        for (int i = 0; i < kTabCount; i++) {
            Cat c = kTabs[i];
            bool sel = (th.cat == c);
            auto t = text(" " + cat_name(c) + " ");
            if (sel) t = t | inverted;
            else     t = t | dim;
            tabs.push_back(t);
            if (i < kTabCount - 1) tabs.push_back(text("  "));
        }
        els.push_back(hbox(std::move(tabs)));

        /* body (single content area; the outer frame draws the one border) */
        Element body_el = text("");
        if (th.cat == Cat::Playback) {
            const char *loops[] = {"顺序播放", "单曲循环", "列表循环", "随机播放"};
            const char *quals[] = {"超清母带", "沉浸环绕", "高清臻音", "Hi-Res",
                                   "无损", "极高", "较高", "标准"};
            const char *qkeys[] = {"jymaster", "sky", "jyeffect", "hires",
                                   "lossless", "exhigh", "higher", "standard"};
            int qidx = 5;  /* default exhigh */
            for (int i = 0; i < 8; i++)
                if (quality == qkeys[i]) { qidx = i; break; }
            Elements body;
            body.push_back(text("  音量:  " + std::to_string(vol) + "   [← / →]"));
            body.push_back(text("  循环:  " + std::string(loops[loop_mode % 4]) + "   [l 切换]"));
            body.push_back(text("  播放音质: " + std::string(quals[qidx]) + "   [q 切换]"));
            body.push_back(text("  快进步长: " + std::to_string(seek) + " 秒  [+ / -]"));
            body.push_back(text(""));
            body.push_back(text("  修改立即保存到 config.json") | dim);
            body_el = vbox(std::move(body)) | flex;
        } else if (th.cat == Cat::Cache) {
            /* audio cache: live size usage + clear action */
            long long total = audio_cache_total_bytes();
            Config *gcfg = config_global();
            int limit_mb = gcfg
                ? config_get_int(gcfg, "cache.audio_limit_mb", 2048) : 2048;
            Elements body;
            body.push_back(text("  音频缓存 (播放时自动缓存, 透明可重建)") | bold);
            body.push_back(separator());
            body.push_back(text("  占用:  " + fmt_capacity(total) +
                                "  /  上限 " + std::to_string(limit_mb) + " MB"));
            body.push_back(text("  目录:  " + std::string(audio_cache_dir())));
            body.push_back(text(""));
            body.push_back(text("  d  清空音频缓存") | bold);
            body.push_back(text("  上限通过 config.json 的 cache.audio_limit_mb 调整 (默认 2048 MB)") | dim);
            body_el = vbox(std::move(body)) | flex;
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
                    body.push_back(hbox({text("> "), text(line) | bold}) | inverted | focus);
                else
                    body.push_back(text(line));
            }
            body.push_back(text(""));
            body.push_back(text("  Enter: 绑定  Backspace: 删除最后绑定  ESC: 返回") | dim);
            body_el = vbox(std::move(body)) | frame | flex;
        } else if (th.mode == Mode::ColorEdit) {
            /* full-screen 2D gradient picker: 纵=色相(滚动) 横=饱和度,
               亮度在底部固定条上, 不参与滚动 */
            const ColorSlot &slot = kSlots[th.slot_sel];
            const ThemeColor &cur = th.theme_edit.*(slot.member);
            ThemeColor cell = pick_cell_color(th.pick_hue, th.pick_sat, th.pick_val);
            Elements body;
            /* live preview: a typed hex (when valid) overrides the cursor color
               so the title swatch shows exactly what will be applied */
            ThemeColor preview = cell;
            if (!th.hex_buf.empty()) {
                std::string ph = th.hex_buf;
                if (ph[0] != '#') ph = "#" + ph;
                ThemeColor t = theme_color_from_hex(ph);
                if (t.has_color) preview = t;
            }
            Elements title;
            title.push_back(text("  编辑颜色 [" + std::string(slot.name) + "]") | bold);
            title.push_back(text("  当前 ") | bold);
            if (cur.has_color) {
                title.push_back(text("  ") | bgcolor(Color::RGB(cur.r, cur.g, cur.b)));
                title.push_back(text(" " + theme_color_to_hex(cur)) | dim);
            } else {
                title.push_back(text("(无色)") | dim);
            }
            title.push_back(text("   预览 ") | bold);
            title.push_back(text("  ") | bgcolor(Color::RGB(preview.r, preview.g, preview.b)));
            title.push_back(text(" " + theme_color_to_hex(preview)) | dim);
            body.push_back(hbox(std::move(title)));
            body.push_back(separator());
            /* scrollable grid: hue row label | sat 0% → 100% */
            Elements rows;
            for (int j = 0; j < HUE_VIEW; j++) {
                int hue_idx = th.hue_top + j;
                if (hue_idx >= HUE_ROWS) break;
                char lbl[8];
                snprintf(lbl, sizeof(lbl), "%3d°", (HUE_ROWS - 1 - hue_idx) * 5);
                Elements line;
                line.push_back(text(lbl) | dim);
                line.push_back(text(" "));
                for (int i = 0; i < SAT_COLS; i++) {
                    ThemeColor c = pick_cell_color(hue_idx, i, th.pick_val);
                    Color rgb = Color::RGB(c.r, c.g, c.b);
                    bool sel = (i == th.pick_sat && hue_idx == th.pick_hue);
                    if (sel) {
                        int luma = 299 * (int)c.r + 587 * (int)c.g + 114 * (int)c.b;
                        Color fg = (luma > 140000) ? Color::Black : Color::White;
                        /* every cell carries an equally-thick border slot so the
                           grid stays aligned and no neighbor covers this frame.
                           The selected cell lights its border up (blank inner
                           cells are unaffected by color(fg)); the others keep a
                           transparent borderEmpty occupying the same slot. */
                        line.push_back(text("  ") | bgcolor(rgb) |
                                       color(fg) | borderLight);
                    } else {
                        line.push_back(text("  ") | bgcolor(rgb) | borderEmpty);
                    }
                }
                rows.push_back(hbox(std::move(line)));
            }
            body.push_back(vbox(std::move(rows)));
            int hue_end = th.hue_top + HUE_VIEW - 1;
            if (hue_end > HUE_ROWS - 1) hue_end = HUE_ROWS - 1;
            int ang_top = (HUE_ROWS - 1 - th.hue_top) * 5;
            int ang_bot = (HUE_ROWS - 1 - hue_end) * 5;
            Element huehint = text("  色相 " + std::to_string(ang_top) +
                                    "°–" + std::to_string(ang_bot) +
                                    "° / 355°   (↑/↓ 或 PgUp/PgDn 滚动)");
            if (th.pick_focus == 0) huehint = huehint | inverted;
            body.push_back(huehint | dim);
            body.push_back(separator());
            /* brightness bar (fixed, never scrolls): 20 cells from 20% to
               100%, sharing the same bordered-slot sizing as the 2D grid so
               the selected cell gets a highlighted frame, not a glyph */
            int bw = 20;
            int vmin = 51;   /* 0.2 * 255 */
            int bpos = (int)((th.pick_val - vmin) / (double)(255 - vmin) *
                             (bw - 1) + 0.5);
            if (bpos < 0) bpos = 0;
            if (bpos > bw - 1) bpos = bw - 1;
            Elements barcells;
            /* leading space mirrors each grid row's leading cell so the bar's
               left edge lines up with the 2D grid's left edge */
            barcells.push_back(text(" "));
            for (int i = 0; i < bw; i++) {
                double frac = (bw > 1) ? i / (double)(bw - 1) : 0.0;
                int v = (int)((vmin + frac * (255 - vmin)) + 0.5);
                int r, g, b;
                hsv_to_rgb((HUE_ROWS - 1 - th.pick_hue + 0.5) * 360.0 / HUE_ROWS,
                           1.0 - 0.80 * th.pick_sat / (SAT_COLS - 1),
                           v / 255.0, &r, &g, &b);
                Color rgb = Color::RGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
                if (i == bpos) {
                    int luma = 299 * r + 587 * g + 114 * b;
                    Color fg = (luma > 140000) ? Color::Black : Color::White;
                    barcells.push_back(text("  ") | bgcolor(rgb) |
                                       color(fg) | borderLight);
                } else {
                    barcells.push_back(text("  ") | bgcolor(rgb) | borderEmpty);
                }
            }
            Element pctline = text("亮度 " +
                                    std::to_string(th.pick_val * 100 / 255) +
                                    "%  [ ] 调节");
            if (th.pick_focus == 1) pctline = pctline | inverted;
            barcells.push_back(pctline | dim);
            body.push_back(hbox(std::move(barcells)));
            /* hex input */
            Element hexline = text("  hex: " + th.hex_buf + "\u258C   (回车应用)");
            if (th.pick_focus == 2) hexline = hexline | inverted;
            body.push_back(hexline | dim);
            body.push_back(text("  Tab 切换焦点(二维表/亮度/hex)   方向键按焦点操作   [ ] 亮度  PgUp/PgDn 翻页  Enter 应用  x 无色  ESC 返回") | dim);
            /* auto-layout: the picker body is vertically centered and the
               grid/brightness cells stretch to fill the terminal width, so
               the window no longer has an empty right or bottom area */
            body_el = vbox({text("") | flex, vbox(std::move(body)) | frame,
                            text("") | flex}) | flex;
        } else if (th.mode == Mode::ThemeEdit) {
            /* theme color-slot editor sub-view */
            Elements body;
            body.push_back(text("  编辑主题: " + th.theme_editing + "   (ESC 保存返回)") | bold);
            body.push_back(separator());
            for (size_t i = 0; i < sizeof(kSlots)/sizeof(kSlots[0]); i++) {
                const ColorSlot &slot = kSlots[i];
                const ThemeColor &c = th.theme_edit.*(slot.member);
                bool sel = ((int)i == th.slot_sel);
                std::string line = "  " + std::string(slot.name) + " = " +
                                   theme_color_to_hex(c);
                if (sel)
                    body.push_back(hbox({text("> "), text(line) | bold}) | inverted | focus);
                else
                    body.push_back(text(line));
            }
            body.push_back(text(""));
            body.push_back(text("  Enter: 编辑颜色  ↑/↓: 选择  ESC: 保存返回") | dim);
            body_el = vbox(std::move(body)) | frame | flex;
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
                        body.push_back(hbox({text("> "), text(line) | bold}) | inverted | focus);
                    else
                        body.push_back(text(line));
                }
            }
            body.push_back(text(""));
            body.push_back(text("  a 添加目录   d 删除选中   (修改立即保存, 重启 netune 生效)") | dim);
            body.push_back(text("  目录列表写入 config.json 的 music_sources.local.dirs") | dim);
            body_el = vbox(std::move(body)) | frame | flex;
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
                    body.push_back(hbox({text("> "), text(line) | bold}) | inverted | focus);
                else
                    body.push_back(text(line));
            }
            body_el = vbox(std::move(body)) | frame | flex;
        }

        /* body into the frame */
        els.push_back(body_el | flex);

        /* bottom: status + hints (dim, separated by a line) */
        std::string hints;
        switch (th.cat) {
            case Cat::Theme:   hints = "Enter 选用   x 编辑颜色   r 重命名   e 导出   d 删除   i 导入   n 新建模板"; break;
            case Cat::Keybind: hints = "Enter 选用(复制为default)   x 编辑按键   r 重命名   e 导出   d 删除   i 导入   n 新建模板"; break;
            case Cat::Layout:  hints = "Enter 选用   r 重命名   e 导出   d 删除   i 导入   n 新建模板"; break;
            case Cat::Main:    hints = "a 添加音乐目录   d 删除选中   ↑/↓ 选择"; break;
            case Cat::Playback:hints = "←/→ 音量   l 循环   + / - 快进步长"; break;
            case Cat::Cache:   hints = "d 清空音频缓存"; break;
        }
        Elements bottom;
        bottom.push_back(th.notice.empty()
            ? text(" ")
            : text(" " + th.notice) | bold);
        bottom.push_back(text(" " + hints) | dim);
        bottom.push_back(text(" ←/→ 或 1-5 切换分类    q 退出") | dim);
        els.push_back(separator());
        els.push_back(vbox(std::move(bottom)));

        return dbox({
            vbox(std::move(els)) | flex | border,
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

        /* ── Full-screen 2D color picker ── */
        if (st.mode == Mode::ColorEdit) {
            std::string k = key_of();
            if (event == Event::Escape) {
                /* while typing hex, ESC cancels the input first; a second
                   ESC leaves the picker */
                if (!st.hex_buf.empty()) { st.hex_buf.clear(); return true; }
                st.mode = Mode::ThemeEdit; return true;
            }
            /* Tab / Shift+Tab cycle focus among 二维表 / 亮度条 / hex */
            if (event == Event::Tab ||
                event == Event::TabReverse) {
                st.pick_focus = event == Event::Tab
                    ? (st.pick_focus + 1) % 3
                    : (st.pick_focus + 2) % 3;
                return true;
            }
            /* arrow keys are routed by the focused region */
            if (k == "left" || k == "right" || k == "up" || k == "down") {
                if (st.pick_focus == 0) {
                    if (k == "left")  st.pick_sat = st.pick_sat > 0 ? st.pick_sat - 1 : 0;
                    if (k == "right") st.pick_sat = st.pick_sat < SAT_COLS - 1 ? st.pick_sat + 1 : SAT_COLS - 1;
                    if (k == "up")    st.pick_hue = st.pick_hue > 0 ? st.pick_hue - 1 : 0;
                    if (k == "down")  st.pick_hue = st.pick_hue < HUE_ROWS - 1 ? st.pick_hue + 1 : HUE_ROWS - 1;
                    /* auto-scroll the viewport to keep the cursor visible */
                    if (st.pick_hue < st.hue_top) st.hue_top = st.pick_hue;
                    if (st.pick_hue >= st.hue_top + HUE_VIEW) st.hue_top = st.pick_hue - HUE_VIEW + 1;
                    st.hex_buf.clear();
                } else if (st.pick_focus == 1) {
                    /* brightness only: left/down darker, right/up brighter */
                    if (k == "left" || k == "down")
                        st.pick_val = st.pick_val > 51 ? st.pick_val - 8 : 51;
                    else
                        st.pick_val = st.pick_val <= 247 ? st.pick_val + 8 : 255;
                    st.hex_buf.clear();
                } /* focus 2 (hex): arrows do nothing */
                return true;
            }
            /* page-scroll the hue viewport (cursor stays visible) */
            if (event == Event::PageUp || event == Event::PageDown) {
                if (st.pick_focus != 0) return true;
                if (event == Event::PageUp) {
                    st.hue_top -= HUE_VIEW;
                    if (st.hue_top < 0) st.hue_top = 0;
                } else {
                    st.hue_top += HUE_VIEW;
                    if (st.hue_top > HUE_ROWS - HUE_VIEW) st.hue_top = HUE_ROWS - HUE_VIEW;
                }
                if (st.pick_hue < st.hue_top) st.pick_hue = st.hue_top;
                if (st.pick_hue >= st.hue_top + HUE_VIEW) st.pick_hue = st.hue_top + HUE_VIEW - 1;
                st.hex_buf.clear();
                return true;
            }
            /* brightness: [ and ] step 5/255 (~2%), ignored while typing hex */
            if (st.pick_focus != 2) {
                if (k == "[") {
                    st.pick_val = st.pick_val > 51 ? st.pick_val - 5 : 51;
                    st.hex_buf.clear();
                    return true;
                }
                if (k == "]") {
                    st.pick_val = st.pick_val <= 250 ? st.pick_val + 5 : 255;
                    st.hex_buf.clear();
                    return true;
                }
            } else if (k == "[" || k == "]") {
                return true;   /* swallow brackets while typing hex */
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
                if (st.pick_focus == 2 && !st.hex_buf.empty()) st.hex_buf.pop_back();
                return true;
            }
            if (event == Event::Return || k == "\r") {
                /* a typed hex is applied verbatim (and an invalid one keeps
                   the user inside the picker instead of dropping context) */
                if (!st.hex_buf.empty()) {
                    std::string hex = st.hex_buf;
                    if (hex[0] != '#') hex = "#" + hex;
                    ThemeColor nc = theme_color_from_hex(hex);
                    if (!nc.has_color) {
                        st.notice = "无效的 hex: " + hex;
                        st.hex_buf.clear();
                        return true;
                    }
                    st.theme_edit.*(kSlots[st.slot_sel].member) = nc;
                    st.dirty_theme = true;
                    st.notice = std::string("已设置 ") + kSlots[st.slot_sel].name +
                                " = " + theme_color_to_hex(nc);
                    st.hex_buf.clear();
                    st.mode = Mode::ThemeEdit;
                    return true;
                }
                /* no hex typed -> apply the grid cursor color */
                ThemeColor nc = pick_cell_color(st.pick_hue, st.pick_sat, st.pick_val);
                st.theme_edit.*(kSlots[st.slot_sel].member) = nc;
                st.dirty_theme = true;
                st.notice = std::string("已设置 ") + kSlots[st.slot_sel].name +
                            " = " + theme_color_to_hex(nc);
                st.mode = Mode::ThemeEdit;
                return true;
            }
            if (k.size() == 1 && st.pick_focus == 2 && st.hex_buf.size() < 9) {
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
                /* place the 2D cursor on the slot's current color */
                const ThemeColor &c = st.theme_edit.*(kSlots[st.slot_sel].member);
                if (c.has_color) {
                    double h, s, v;
                    rgb_to_hsv(c.r, c.g, c.b, &h, &s, &v);
                    st.pick_hue = HUE_ROWS - 1 -
                                  (((int)(h / 360.0 * HUE_ROWS)) % HUE_ROWS);
                    st.pick_sat = (int)((1.0 - s) / 0.80 * (SAT_COLS - 1) + 0.5);
                    if (st.pick_sat < 0) st.pick_sat = 0;
                    if (st.pick_sat > SAT_COLS - 1) st.pick_sat = SAT_COLS - 1;
                    st.pick_val = (int)(v * 255 + 0.5);
                    if (st.pick_val > 255) st.pick_val = 255;
                    /* center the viewport on the cursor */
                    st.hue_top = st.pick_hue - HUE_VIEW / 2;
                    if (st.hue_top < 0) st.hue_top = 0;
                    if (st.hue_top > HUE_ROWS - HUE_VIEW) st.hue_top = HUE_ROWS - HUE_VIEW;
                } else {
                    st.pick_hue = 0;
                    st.pick_sat = 0;
                    st.pick_val = 200;
                    st.hue_top = 0;
                }
                st.mode = Mode::ColorEdit;
                st.pick_focus = 0;   /* start on the 2D grid */
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
                } else if (st.confirm_kind == 3) {
                    /* clear the audio cache */
                    int removed = audio_cache_clear();
                    st.notice = removed > 0
                        ? "已清空音频缓存 (" + std::to_string(removed) + " 个文件)"
                        : "音频缓存已是空的";
                } else {
                    std::string target = dir_of(st.cat) + "/" + st.files[st.sel].name;
                    if (st.confirm_kind == 0) {
                        if (remove_utf8(target.c_str()) == 0) {
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
                    if (rename_utf8(src.c_str(), dst.c_str()) == 0) {
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
        if (k == "d" && st.cat == Cat::Cache) {
            st.confirm_kind = 3;
            st.confirm_msg = "确认清空所有音频缓存? (y/n)";
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
        if (k == "q" && st.cat == Cat::Playback) {
            static const char *const qkeys[] = {"jymaster", "sky", "jyeffect", "hires",
                                                "lossless", "exhigh", "higher", "standard"};
            int qidx = 5;  /* default exhigh */
            for (int i = 0; i < 8; i++)
                if (quality == qkeys[i]) { qidx = i; break; }
            qidx = (qidx + 1) % 8;
            quality = qkeys[qidx];
            if (cfg) {
                config_set_str(cfg, "netease.quality", quality.c_str());
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
