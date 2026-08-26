#include "ui/components/action_sheet.h"
#include "ui/components/theme_util.h"
#include "ui/components/spinner.h"
#include "ui/theme.h"
#include <string>
#include <vector>
#include <chrono>
using namespace ftxui;

/* ── Option rows for the main action menu (menu 0) ── */
struct ActionOpt { std::string label; int id; };

Element render_action_sheet(const AppState &s) {
    if (!s.action_sheet_open) return text("");

    const auto &item = s.playlist.empty() ? SongInfo{} :
                       s.playlist[s.selected_index];
    bool is_playlist = item.is_playlist;
    bool is_vip = (!is_playlist && item.fee == 1);
    std::string title = item.title ? item.title : "(nothing selected)";
    if (s.action_sheet_menu == 4)
        title = "\u4E0B\u8F7D\u97F3\u8D28: " + title;  /* 下载音质: <song> */
    if (s.action_sheet_menu == 6)
        title = "\u64AD\u653E\u97F3\u8D28: " + title;   /* 播放音质: <song> */
    auto &th = ThemeManager::instance().current();

    Elements body;

    if (s.action_sheet_menu == 0) {
        /* main menu — options depend on the selected object:
           playlist:  subscribe/unsubscribe (any) + rename/delete (own only)
           song:      like/unlike (any) + add-to-playlist (any) +
                     remove-from-current (own playlist detail only) */
        std::vector<ActionOpt> opts;
        if (is_playlist) {
            opts.push_back({s.action_sheet_active == 1
                ? " \u2606 \u53D6\u6D88\u6536\u85CF\u6B4C\u5355 "   /* ☆ 取消收藏歌单 */
                : " \u2606 \u6536\u85CF\u6B4C\u5355 ", 1});       /* ☆ 收藏歌单 */
            if (item.mine == 1) {
                opts.push_back({" \u270E \u91CD\u547D\u540D... ", 2});     /* ✎ 重命名... */
                opts.push_back({" \u2716 \u5220\u9664\u6B4C\u5355 ", 3});  /* ✖ 删除歌单 */
            }
        } else {
            opts.push_back({s.action_sheet_active == 1
                ? " \u2665 \u53D6\u6D88\u559C\u6B22 "   /* ♥ 取消喜欢 */
                : " \u2665 \u559C\u6B22\u6B64\u6B4C\u66F2 ", 1});  /* ♥ 喜欢此歌曲 */
            opts.push_back({" \u21D2 \u6536\u85CF\u5230\u6B4C\u5355... ", 4});  /* ⇒ 收藏到歌单... */
            opts.push_back({" \u2B07 \u4E0B\u8F7D ", 6});   /* ⬇ 下载 */
            opts.push_back({" \u266B \u64AD\u653E\u97F3\u8D28 ", 8}); /* ♫ 播放音质 */
            opts.push_back({" \u2139 \u6B4C\u66F2\u8BE6\u60C5 ", 7});  /* ℹ 歌曲详情 */
            if (s.detail_playlist_mine) {
                opts.push_back({" \u2716 \u4ECE\u5F53\u524D\u6B4C\u5355\u79FB\u9664 ", 5}); /* ✖ 从当前歌单移除 */
            }
        }
        for (size_t i = 0; i < opts.size(); i++) {
            auto row = text(opts[i].label);
            if ((int)i == s.action_sheet_selected)
                body.push_back(hbox({theme_sel_marker(is_playlist, is_vip), row | bold}) | focus);
            else
                body.push_back(hbox({text("  "), row}));
        }
    } else if (s.action_sheet_menu == 1) {
        /* playlist picker */
        body.push_back(text(" \u9009\u62E9\u6B4C\u5355: ") | bold);  /* 选择歌单: */
        if (s.action_sheet_pls.empty()) {
            body.push_back(text("  (\u52A0\u8F7D\u4E2D...)") | dim);  /* (加载中...) */
        } else {
            for (size_t i = 0; i < s.action_sheet_pls.size(); i++) {
                auto row = text(std::string("  ") +
                                (s.action_sheet_pls[i].title ? s.action_sheet_pls[i].title : ""));
                if ((int)i == s.action_sheet_selected)
                    body.push_back(hbox({theme_sel_marker(true, false), row | bold}) | focus);
                else
                    body.push_back(hbox({text("  "), row}));
            }
        }
    } else if (s.action_sheet_menu == 2) {
        /* text input */
        std::string label = s.action_sheet_ctx == "rename"
            ? " \u91CD\u547D\u540D: "        /* 重命名: */
            : " \u65B0\u5EFA\u6B4C\u5355: "; /* 新建歌单: */
        body.push_back(text(label) | bold);
        body.push_back(text(" \u2502" + s.action_sheet_input + "\u258C\u2502 ") | bold
                       | bgcolor(Color::RGB(th.accent.r, th.accent.g, th.accent.b))
                       | color(Color::RGB(0, 0, 0)));
        body.push_back(text("  Enter \u786E\u8BA4  Esc \u53D6\u6D88 ") | dim);
    } else if (s.action_sheet_menu == 3) {
        /* confirm */
        body.push_back(text(" \u786E\u8BA4\u5220\u9664\u6B4C\u5355? ") | bold
                       | color(Color::RGB(th.error.r, th.error.g, th.error.b)));
        body.push_back(text("  Enter \u786E\u8BA4  Esc \u53D6\u6D88 ") | dim);
    } else if (s.action_sheet_menu == 4) {
        /* download quality picker: high→low. Rows follow the track's
           per-tier source table: -2 = no source (hidden), -1 = probing,
           1 = exists (shown with bitrate), 0 = exists but denied. While
           the source probe runs the list is replaced by a spinner.
           After a level is chosen the sheet stays open (no auto-close),
           and the live download state for each tier (from s.downloads,
           matched by song id + level) is shown on the row's right edge:
           spinner + % while downloading, a queued marker while waiting. */
        if (s.action_sheet_quality_probing) {
            /* time-based spinner frames (independent of a stale start) */
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
            const char *f = frames[(now_ms / 16) % 10];
            body.push_back(hbox({text(" " + std::string(f) + " "),
                                 text("Loading...") | dim}));
            body.push_back(text("  \u6B63\u5728\u68C0\u6D4B\u97F3\u8D28\u6E90...") | dim);  /* 正在检测音质源... */
        } else {
        static const char *const kNames[] = {
            "\u8D85\u6E05\u6BCD\u5E26",      /* 超清母带 jymaster */
            "\u6C89\u6D78\u73AF\u7ED5",      /* 沉浸环绕 sky */
            "\u9AD8\u6E05\u81FB\u97F3",      /* 高清臻音 jyeffect */
            "Hi-Res",                         /* hires */
            "\u65E0\u635F",                   /* 无损 lossless */
            "\u6781\u9AD8",                   /* 极高 exhigh */
            "\u8F83\u9AD8",                   /* 较高 higher */
            "\u6807\u51C6"                    /* 标准 standard */
        };
        static const char *const kLevels[] = {
            "jymaster", "sky", "jyeffect", "hires",
            "lossless", "exhigh", "higher", "standard"
        };
        int count = s.action_sheet_quality_count > 0
                    ? s.action_sheet_quality_count : 8;
        for (int i = 0; i < count; i++) {
            int st = (int)s.action_sheet_quality_ok.size() > i
                     ? s.action_sheet_quality_ok[i] : -1;
            if (st == -2) continue;  /* no source — don't show the row */
            std::string label = " " + std::string(kNames[i]);
            if (st == -1) {
                label += " (\u68C0\u6D4B\u4E2D...)";  /* (检测中...) */
            } else {
                int br = (int)s.action_sheet_quality_br.size() > i
                         ? s.action_sheet_quality_br[i] : 0;
                if (br > 0) {
                    /* format bitrate: <1000 → k, >=1000 → e.g. 1.77M */
                    char bbuf[32];
                    if (br >= 1000000) snprintf(bbuf, sizeof bbuf, " %.2fM",
                                                br / 1000000.0);
                    else               snprintf(bbuf, sizeof bbuf, " %dk", br / 1000);
                    label += std::string(bbuf);
                }
            }
            /* live download state for this tier (song + level match) */
            std::string dl_tail;
            for (const auto &d : s.downloads) {
                if (d.id != s.action_sheet_quality_id) continue;
                if (d.level != kLevels[i]) continue;
                if (d.status == DlStatus::Downloading) {
                    int p = d.total > 0 ? (int)(d.done * 100 / d.total) : 0;
                    if (p > 100) p = 100;
                    char pb[16];
                    snprintf(pb, sizeof pb, " %d%%", p);
                    dl_tail = std::string(" ") + spinner_glyph() + pb;
                } else if (d.status == DlStatus::Queued) {
                    dl_tail = " \u6392\u961F";  /* 排队 */
                }
                break;
            }
            auto row = text(label);
            if ((int)i == s.action_sheet_selected)
                row = hbox({theme_selection(text(" \u203A ")), row | bold}) | focus;
            else
                row = hbox({text("   "), row});
            if (!dl_tail.empty())
                row = hbox({row, filler(), theme_fg(text(dl_tail))});
            if (st == 2) {
                /* denied tier (no entitlement / server refuses): mark ONLY
                   the failing tiers, and change the character colour
                   (warning fg) — the row background stays the sheet's. */
                row = theme_warning(row);
            }
            body.push_back(row);
        }
        }
    } else if (s.action_sheet_menu == 6) {
        /* play-quality picker: per-song override for streaming. Rows are
           the 8 levels high→low; only tiers with a real source are shown
           (-2 = no source, hidden), -1 = still probing. The currently
           effective tier (override or global) is marked. Enter sets the
           override, G sets the global default. */
        if (s.action_sheet_quality_probing) {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
            const char *f = frames[(now_ms / 16) % 10];
            body.push_back(hbox({text(" " + std::string(f) + " "),
                                 text("Loading...") | dim}));
            body.push_back(text("  \u6B63\u5728\u68C0\u6D4B\u97F3\u8D28\u6E90...") | dim);  /* 正在检测音质源... */
        } else {
        static const char *const kNames[] = {
            "\u8D85\u6E05\u6BCD\u5E26",      /* 超清母带 jymaster */
            "\u6C89\u6D78\u73AF\u7ED5",      /* 沉浸环绕 sky */
            "\u9AD8\u6E05\u81FB\u97F3",      /* 高清臻音 jyeffect */
            "Hi-Res",                         /* hires */
            "\u65E0\u635F",                   /* 无损 lossless */
            "\u6781\u9AD8",                   /* 极高 exhigh */
            "\u8F83\u9AD8",                   /* 较高 higher */
            "\u6807\u51C6"                    /* 标准 standard */
        };
        int count = s.action_sheet_quality_count > 0
                    ? s.action_sheet_quality_count : 8;
        for (int i = 0; i < count; i++) {
            int st = (int)s.action_sheet_quality_ok.size() > i
                     ? s.action_sheet_quality_ok[i] : -1;
            if (st == -2) continue;  /* no source — don't show the row */
            std::string label = " " + std::string(kNames[i]);
            if (st == -1)
                label += " (\u68C0\u6D4B\u4E2D...)";  /* (检测中...) */
            else if (st == 1)
                label += " \u2713";  /* ✓ current */
            auto row = text(label);
            if ((int)i == s.action_sheet_selected)
                row = hbox({theme_selection(text(" \u203A ")), row | bold}) | focus;
            else
                row = hbox({text("   "), row});
            if (st == 1)
                row = row | color(Color::RGB(th.accent.r, th.accent.g, th.accent.b));
            else if (st == 2)
                row = theme_warning(row);  /* denied tier: fg colour only,
                                              background stays the sheet's */
            /* st == 0 (playable, not current) stays in the default colour —
               no VIP marker colour any more. */
            body.push_back(row);
        }
        body.push_back(text("  \u56DE\u8F66\u8BBE\u6B64\u66F2\u97F3\u8D28  G \u8BBE\u5168\u5C40\u9ED8\u8BA4  Esc \u53D6\u6D88 ") | dim);
        /* 回车设此曲音质  G 设全局默认  Esc 取消 */
        }
    } else if (s.action_sheet_menu == 5) {
        /* song detail (was the d-key popup, merged into the action sheet) */
        if (s.song_detail_lines.empty()) {
            body.push_back(text("  (\u52A0\u8F7D\u4E2D...)") | dim);  /* (加载中...) */
        } else {
            for (const auto &l : s.song_detail_lines) {
                if (!l.empty() && l[0] == '\t')
                    body.push_back(text(l.substr(1)) | color(Color::RGB(255,255,255)));
                else
                    body.push_back(text(l));
            }
        }
        body.push_back(text("  Esc \u8FD4\u56DE ") | dim);  /* Esc 返回 */
    }

    /* Theme-driven popup chrome: background/text/selection/frame all come
       from the theme system (overlay_bg, fg, accent_bg, border) instead of
       hardcoded RGB — following the help-screen pattern (border inside,
       overlay bg applied outermost). */
    auto box = vbox({
        theme_accent(text(" " + title + " ") | bold),
        separator(),
        theme_fg(vbox(std::move(body))),
    }) | borderRounded;
    box = theme_popup_border(box);
    box = theme_overlay_bg(box);

    /* Cap the width to the song panel: an over-wide row (long titles)
       would otherwise overflow the panel, break the right alignment
       and leave the border columns misplaced (stray vertical lines). */
    int max_w = s.top_row_width - 24;
    if (max_w < 40) max_w = 40;
    box = box | size(WIDTH, LESS_THAN, max_w - 1);

    /* Bottom-right of the song-list panel: the popup's bottom edge
       lands on the panel border's bottom row (y = H-3) so its border
       corner meets the list's border corner seamlessly. */
    int n = s.screen_height - 4;
    if (n < 3) n = 3;
    return vbox({filler(), hbox({filler(), box})})
        | size(HEIGHT, LESS_THAN, n);
}
