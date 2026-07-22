#include "ui/components/login_screen.h"
#include "ui/components/theme_util.h"
#include "ui/theme.h"
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
    Elements col;

    /* Title */
    col.push_back(text(" Netease Login ") | bold | center | underlined);
    col.push_back(separator());

    switch (s.login_state) {
    case 1:
        col.push_back(filler());
        col.push_back(theme_fg(text(" Connecting... ")) | center);
        col.push_back(filler());
        break;

    case 2: {
        /* QR code — takes up most of the screen */
        col.push_back(filler());
        if (!s.login_qr.empty()) {
            auto qr_lines = split_lines(s.login_qr);
            Elements qr_els;
            for (auto &ln : qr_lines)
                qr_els.push_back(text("  " + ln));
            col.push_back(vbox(std::move(qr_els)) | center);
        }
        col.push_back(filler());
        /* Instructions — shown after QR */
        col.push_back(theme_accent(text(" Scan with Netease Music App ") | bold) | center);
        col.push_back(text(" 1. Open NCM on your phone ") | dim | center);
        col.push_back(text(" 2. Tap the scan icon (top-right) ") | dim | center);
        col.push_back(text(" 3. Scan this code ") | dim | center);
        break;
    }

    case 3:
        col.push_back(filler());
        col.push_back(theme_accent(text(" Login successful! ")) | bold | center);
        if (!s.login_status.empty())
            col.push_back(theme_accent(text(" " + s.login_status + " ")) | center);
        col.push_back(filler());
        break;

    case -1:
        col.push_back(filler());
        col.push_back(text(" Error: " + s.login_status) | center);
        col.push_back(filler());
        break;
    }

    /* Bottom status */
    col.push_back(separator());
    if (s.login_state == 2) {
        /* Show status text if set; otherwise default messages */
        const char *bottom = " Waiting for scan... [Esc] cancel ";
        if (s.login_status.find("Scanned") != std::string::npos ||
            s.login_status.find("Confirm") != std::string::npos)
            bottom = " Scanned! Confirm in app... [Esc] cancel ";
        col.push_back(theme_fg(text(bottom)) | dim | center);
    } else {
        col.push_back(theme_fg(text(" [Esc] back ")) | dim | center);
    }

    auto page = vbox(std::move(col));
    /* yframe + flex: fills available height, adds scrollbar if needed */
    auto &theme = ThemeManager::instance().current();
    return page | yframe | flex | bgcolor(Color::RGB(theme.overlay_bg.r, theme.overlay_bg.g, theme.overlay_bg.b));
}
