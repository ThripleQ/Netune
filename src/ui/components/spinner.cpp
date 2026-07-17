#include "ui/components/spinner.h"
#include "ui/components/theme_util.h"
#include <string>
#include <thread>
#include <chrono>
using namespace ftxui;

Element render_spinner(const AppState &s) {
    static int frame = 0;
    static bool was_loading = false;

    if (!s.loading && !s.cover_loading) {
        was_loading = false;
        return text("");
    }

    /* reset on new loading session */
    if (!was_loading) {
        frame = 0;
        was_loading = true;
    }

    frame++;
    const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    auto &f = frames[(frame / 5) % 10];
    return hbox({
        text(" " + std::string(f) + " "),
        text("Loading...") | dim,
    }) | center;
}
