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
    {Action::PanelSwitch,  "切换面板 (歌单/歌曲)"},
    {Action::MoveDown,     "向下移动"},
    {Action::MoveUp,       "向上移动"},
    {Action::PlaySelected, "播放选中项"},
    {Action::OpenSearch,   "搜索"},
};

static const HelpEntry kPlay[] = {
    {Action::PlayPause,    "播放 / 暂停"},
    {Action::NextTrack,    "下一首"},
    {Action::PrevTrack,    "上一首"},
    {Action::SeekForward,  "快进"},
    {Action::SeekBackward, "快退"},
    {Action::Stop,         "停止播放"},
};

static const HelpEntry kVol[] = {
    {Action::VolumeUp,     "音量 +"},
    {Action::VolumeDown,   "音量 -"},
    {Action::ToggleMute,   "静音"},
};

static const HelpEntry kMisc[] = {
    {Action::CycleLoop,    "循环模式"},
    {Action::ToggleLyrics, "歌词"},
    {Action::ShowHelp,     "打开 / 关闭本帮助"},
    {Action::ShowActions,  "操作小窗 (喜欢/收藏歌单)"},
    {Action::ShowSongDetail, "歌曲详情"},
    {Action::Quit,         "退出"},
};

/* ── Theme slot → UI element mapping (what each color controls) ── */
struct ThemeSlotInfo {
    const char *ui_element;  /* what the color controls in the UI */
    const char *slot;        /* config key under colors: */
    ThemeColor Theme::*member;
};

static const ThemeSlotInfo kSlots[] = {
    {"界面背景",   "bg",             &Theme::bg},
    {"正文文字",   "fg",             &Theme::fg},
    {"强调 / 标题", "accent",         &Theme::accent},
    {"选中行背景",  "accent_bg",      &Theme::accent_bg},
    {"次要文字",   "muted",          &Theme::muted},
    {"边框",      "border",          &Theme::border},
    {"成功提示",   "success",        &Theme::success},
    {"警告色",    "warning",         &Theme::warning},
    {"错误提示",   "error",          &Theme::error},
    {"弹窗背景",   "overlay_bg",     &Theme::overlay_bg},
    {"进度条轨道",  "progress_track", &Theme::progress_track},
    {"频谱",      "spectrum",        &Theme::spectrum},
    {"VIP 标记",  "vip",             &Theme::vip},
    {"歌单标记",   "playlist",        &Theme::playlist},
    {"网易云Logo", "logo",            &Theme::logo},
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
    constexpr int KEYW = 11;

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

    /* numbered groups so the reading order is obvious */
    auto group = [&](const char *title, const HelpEntry *items, size_t n) {
        Elements col;
        col.push_back(theme_accent(text(title) | bold));
        col.push_back(text(""));
        for (size_t i = 0; i < n; i++)
            col.push_back(entry(items[i]));
        return vbox(std::move(col));
    };

    auto nav  = group(" 1 导航 ",        kNav,  sizeof(kNav)  / sizeof(kNav[0]));
    auto play = group(" 2 播放 ",        kPlay, sizeof(kPlay) / sizeof(kPlay[0]));
    auto vol  = group(" 3 音量 ",        kVol,  sizeof(kVol)  / sizeof(kVol[0]));
    auto misc = group(" 4 其他 ",        kMisc, sizeof(kMisc) / sizeof(kMisc[0]));

    /* Left column: two groups top, two below, balanced */
    auto left_col = vbox(Elements{ nav, filler(), play, filler(), vol, filler(), misc });

    /* Right column: theme slot legend — swatch + UI element + slot key */
    auto &t = ThemeManager::instance().current();
    Elements legend;
    legend.push_back(theme_accent(text(" 主题元素 (颜色槽) ") | bold));
    legend.push_back(text(""));
    for (auto &slot : kSlots) {
        const ThemeColor &c = t.*(slot.member);
        Element sw = c.has_color
            ? text("  ") | bgcolor(Color::RGB(c.r, c.g, c.b))
            : text(" · ");
        legend.push_back(hbox(Elements{
            sw,
            text("  " + std::string(slot.ui_element) + "  →  " + slot.slot),
        }));
    }
    legend.push_back(text(""));
    legend.push_back(text("  修改位置: netune-config → 主题 → x 编辑") | dim);
    auto right_col = vbox(std::move(legend));

    auto body = hbox(Elements{
        left_col | flex,
        text("     "),
        vbox(Elements{
            separatorEmpty(),
            right_col | flex,
        }) | flex,
    });

    auto &theme = ThemeManager::instance().current();
    auto content = vbox(Elements{
        filler(),
        theme_accent(text(" Help ") | bold),
        theme_border(body | border),
        text(" 按 ? 或 Esc 关闭 ") | dim | center,
        filler(),
    });
    return theme_overlay_bg(content | yframe | flex);
}
