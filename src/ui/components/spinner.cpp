#include "ui/components/spinner.h"
#include "ui/components/theme_util.h"
#include <string>
#include <chrono>
using namespace ftxui;
using namespace std::chrono;

std::string spinner_glyph(void) {
    auto now_ms = duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
    static const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return frames[(int)((now_ms / 16) % 10)];
}

Element render_spinner(const AppState &s) {
    static auto start = steady_clock::now();
    static bool was_loading = false;

    if (!s.loading && !s.cover_loading) {
        was_loading = false;
        return text("");
    }

    if (!was_loading) {
        start = steady_clock::now();
        was_loading = true;
    }

    /* Time-based frame: 80ms per frame → ~12.5 fps, smooth enough */
    int elapsed = (int)duration_cast<milliseconds>(steady_clock::now() - start).count();
    int idx = (elapsed / 16) % 10;
    const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    auto &f = frames[idx];
    return hbox({
        text(" " + std::string(f) + " "),
        text("Loading...") | dim,
    }) | center;
}
