#include "ui/components/login_screen.h"
#include "ui/components/theme_util.h"
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>
using namespace ftxui;

/* Split QR code string into lines */
static std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> lines;
    std::istringstream stream(s);
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);
    return lines;
}

Element render_login_screen(const AppState &s) {
    /* Build the full login page */
    Elements col;

    /* Title bar */
    col.push_back(text(" Netease Cloud Music - Login ") | bold | center | underlined);
    col.push_back(separator());
    col.push_back(text("")); /* spacer */

    switch (s.login_state) {
    case 1: /* Getting QR key */
        col.push_back(theme_fg(text(" Connecting to server... ")) | center);
        col.push_back(text(""));
        col.push_back(text(" Please wait ") | dim | center);
        break;

    case 2: /* QR displayed, waiting for scan */
        if (!s.login_qr.empty()) {
            auto qr_lines = split_lines(s.login_qr);
            Elements qr_els;
            for (auto &ln : qr_lines) {
                /* Add a leading space for centering offset */
                qr_els.push_back(text("  " + ln));
            }
            col.push_back(vbox(std::move(qr_els)) | center);
        } else {
            col.push_back(text(""));
        }
        col.push_back(text(""));
        col.push_back(theme_accent(text(" >>> Scan with Netease Music App <<< ") | bold) | center);
        col.push_back(text(""));
        col.push_back(text(" 1. Open Netease Cloud Music on your phone ") | dim | center);
        col.push_back(text(" 2. Tap the scan icon in the top-right corner ") | dim | center);
        col.push_back(text(" 3. Scan this QR code ") | dim | center);
        break;

    case 3: /* Logged in */
        col.push_back(text(""));
        col.push_back(theme_accent(text(" ✓ Login successful! ")) | bold | center);
        col.push_back(text(""));
        if (!s.login_status.empty()) {
            col.push_back(theme_accent(text(" Welcome, " + s.login_status + " ")) | center);
        }
        break;

    case -1: /* Error */
        col.push_back(theme_fg(text("")) | center);
        break;
    }

    /* Status line */
    col.push_back(filler());
    col.push_back(separator());
    if (!s.login_status.empty() && s.login_state != 2) {
        col.push_back(theme_fg(text(" " + s.login_status)) | dim | center);
    }
    col.push_back(text(" [Esc] back ") | dim | center);

    auto page = vbox(std::move(col));
    return page | bgcolor(Color::RGB(15, 15, 25));
}
