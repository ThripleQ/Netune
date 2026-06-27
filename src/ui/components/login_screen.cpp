#include "ui/components/login_screen.h"
#include "ui/components/theme_util.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace ftxui;

Element render_login_screen(const AppState &s) {
    if (s.login_state == 0) return text(""); /* not active */

    Elements col;

    /* Title */
    col.push_back(text(" Netease Login ") | bold | center | underlined);
    col.push_back(separator());

    switch (s.login_state) {
    case 1: /* Getting QR key */
        col.push_back(theme_fg(text(" Getting QR code... ")) | center);
        break;

    case 2: /* QR code displayed, waiting for scan */
        if (!s.login_qr.empty()) {
            /* Display QR code (text-based, from qrencode) */
            col.push_back(text(s.login_qr) | center);
            col.push_back(separator());
        } else {
            col.push_back(theme_fg(text(" Scan with Netease Music App ")) | center);
        }
        col.push_back(theme_accent(text(" > ") | bold) | center);
        col.push_back(theme_fg(text(" Please scan the QR code using ")) | center);
        col.push_back(theme_fg(text(" Netease Cloud Music mobile app ")) | center);
        col.push_back(separator());
        break;

    case 3: /* Logged in */
        if (!s.login_status.empty()) {
            col.push_back(theme_accent(text(" ✓ " + s.login_status)) | bold | center);
        } else {
            col.push_back(theme_accent(text(" ✓ Logged in! ")) | bold | center);
        }
        break;

    case -1: /* Error */
        col.push_back(theme_fg(text(" ✗ " + s.login_status)) | center);
        break;
    }

    /* Status message */
    if (!s.login_status.empty() && s.login_state != 3 && s.login_state != 2) {
        col.push_back(theme_fg(text(" " + s.login_status)) | dim | center);
    }

    col.push_back(separator());
    col.push_back(text(" [Esc] close ") | dim | center);

    auto box = vbox(std::move(col));
    return box | border | center | clear_under | bgcolor(Color::RGB(15, 15, 25));
}
