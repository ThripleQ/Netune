#include "ui/components/spinner.h"
#include "ui/components/theme_util.h"
#include <string>
#include <thread>
#include <chrono>
using namespace ftxui;

Element render_spinner(const AppState &s) {
    if (!s.loading) return text("");
    static int frame = 0;
    frame++;
    const char *frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    auto &f = frames[(frame / 8) % 10];
    return hbox({
        text(" " + std::string(f) + " "),
        text("Loading...") | dim,
    }) | center;
}
