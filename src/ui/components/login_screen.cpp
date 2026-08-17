#include "ui/components/login_screen.h"
#include "ui/components/theme_util.h"
#include "ui/theme.h"
#include "core/term_gfx.h"
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
using namespace ftxui;
using namespace std::chrono;

/* Split QR code string into lines */
static std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> lines;
    std::istringstream stream(s);
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);
    return lines;
}

/* Time-based spinner frame (independent of the song-loading spinner) */
static Element login_spinner_el(void) {
    static auto start = steady_clock::now();
    int elapsed = (int)duration_cast<milliseconds>(steady_clock::now() - start).count();
    int idx = (elapsed / 16) % 10;
    static const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return text(frames[idx]);
}

static Element qr_box(const std::string &qr_text) {
    auto qr_lines = split_lines(qr_text);
    Elements qr_els;
    for (auto &ln : qr_lines)
        qr_els.push_back(text(ln));
    return vbox(std::move(qr_els)) | borderRounded;
}

Element render_login_screen(const AppState &s) {
    /* Natural size of the character QR in terminal cells (indent +
       border included). Module rows collapse 2:1 into half-block text
       rows, so this is the MINIMUM window size that renders every
       module — below it the code is clipped and cannot be scanned. */
    auto qr_min_dims = [](const std::string &qr_text, int *cols, int *rows) {
        if (qr_text.empty()) return false;
        size_t first = qr_text.find('\n');
        int w = (first == std::string::npos) ? (int)qr_text.size() : (int)first;
        int h = 1;
        for (size_t i = 0; i < qr_text.size(); i++)
            if (qr_text[i] == '\n') h++;
        *cols = w + 4;  /* 2-space indent + border */
        *rows = h + 2;  /* border */
        return true;
    };

    Elements col;

    /* Title */
    col.push_back(text(" Netease Login ") | bold | center | underlined);
    col.push_back(separator());

    switch (s.login_state) {
    case 1:
        col.push_back(filler());
        col.push_back(hbox({login_spinner_el(), text(" Connecting... ")}) | center);
        col.push_back(filler());
        break;

    case 2: {
        /* QR code — centered, boxed. With kitty graphics the real image
           is placed by app.cpp at row 3; reserve an invisible fixed
           placeholder so the countdown stays anchored. The kitty path
           resamples the full-resolution PNG, so it never loses modules
           at any size — only the character path has a hard minimum. */
        if (!s.login_qr.empty() && term_gfx_active()) {
            int rows = s.screen_height - 8;
            if (rows < 4) rows = 4;
            if (rows > 12) rows = 12;
            col.push_back(vbox(Elements{}) | size(HEIGHT, EQUAL, rows));
        } else if (!s.login_qr.empty()) {
            int need_w = 0, need_h = 0;
            if (!qr_min_dims(s.login_qr, &need_w, &need_h) ||
                s.top_row_width < need_w || s.screen_height < need_h) {
                /* too small: rendering would clip modules — refuse and
                   tell the user the minimum size instead of showing a
                   broken code */
                std::string msg = " Terminal too small: QR needs ";
                if (need_w > 0 || need_h > 0)
                    msg += std::to_string(need_w) + "x" + std::to_string(need_h);
                msg += " — enlarge the window ";
                col.push_back(theme_fg(text(msg)) | center);
            } else {
                col.push_back(qr_box(s.login_qr) | center);
            }
        } else {
            col.push_back(theme_fg(text(" No QR code available ")) | center);
        }

        /* Countdown below the code */
        long remain = s.login_qr_deadline - (long)time(NULL);
        if (remain > 0) {
            bool urgent = remain <= 15;
            auto txt = text(" Code expires in " + std::to_string(remain) + "s ") | dim;
            if (urgent) {
                auto &th = ThemeManager::instance().current();
                txt = text(" Code expires in " + std::to_string(remain) + "s — refresh soon ")
                      | bold | color(Color::RGB(th.error.r, th.error.g, th.error.b));
            }
            col.push_back(txt | center);
        }

        /* Instructions */
        bool scanned = s.login_status.find("Scanned") != std::string::npos ||
                       s.login_status.find("Confirm") != std::string::npos;
        col.push_back(filler());
        if (scanned) {
            auto &th = ThemeManager::instance().current();
            /* full-body feedback: replace the steps with a prominent
               "scanned" state so the phone confirmation is obvious */
            col.push_back(hbox({
                login_spinner_el(),
                text(" Scanned! ") | bold
                    | color(Color::RGB(th.accent.r, th.accent.g, th.accent.b)),
            }) | center);
            col.push_back(text(" Confirm login in the app on your phone ") | bold | center);
            col.push_back(separator());
            col.push_back(text(" 扫码成功，请在手机上点击确认 ") | dim | center);
        } else {
            col.push_back(theme_accent(text(" Scan with Netease Music App ") | bold) | center);
            col.push_back(text(" 1. Open the app on your phone ") | dim | center);
            col.push_back(text(" 2. Tap the scan icon (top-right) ") | dim | center);
            col.push_back(text(" 3. Scan this code ") | dim | center);
        }
        break;
    }

    case 3: {
        auto &th = ThemeManager::instance().current();
        col.push_back(filler());
        col.push_back(text(" ✓ ") | bold | color(Color::RGB(th.accent.r, th.accent.g, th.accent.b))
                      | center);
        col.push_back(text(" Login successful! ") | bold | center);
        if (!s.login_status.empty())
            col.push_back(theme_accent(text(" " + s.login_status + " ")) | center);
        col.push_back(hbox({
            login_spinner_el(),
            text(" Loading your playlists... ") | dim,
        }) | center);
        col.push_back(filler());
        break;
    }

    case -1: {
        auto &th = ThemeManager::instance().current();
        col.push_back(filler());
        col.push_back(text(" Error: " + s.login_status)
                      | bold | color(Color::RGB(th.error.r, th.error.g, th.error.b)) | center);
        col.push_back(filler());
        break;
    }
    }

    /* Bottom status */
    col.push_back(separator());
    if (s.login_state == 2) {
        auto &th = ThemeManager::instance().current();
        if (s.login_net_error) {
            col.push_back(hbox({
                login_spinner_el(),
                text(" Network error, retrying... [Esc] cancel ") | dim,
            }) | center);
        } else {
            bool scanned = s.login_status.find("Scanned") != std::string::npos ||
                           s.login_status.find("Confirm") != std::string::npos;
            std::string bottom = scanned
                ? " Scanned! Confirm in app... [Esc] cancel "
                : " Waiting for scan... [Esc] cancel ";
            auto el = text(bottom) | dim;
            if (scanned)
                el = text(bottom) | color(Color::RGB(th.accent.r, th.accent.g, th.accent.b));
            col.push_back(el | center);
        }
    } else {
        col.push_back(theme_fg(text(" [Esc] back ")) | dim | center);
    }

    auto page = vbox(std::move(col));
    /* yframe + flex: fills available height, adds scrollbar if needed */
    auto &theme = ThemeManager::instance().current();
    return page | yframe | flex | bgcolor(Color::RGB(theme.overlay_bg.r, theme.overlay_bg.g, theme.overlay_bg.b));
}