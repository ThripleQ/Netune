#include "ui/components/group_list.h"
#include "ui/components/theme_util.h"
using namespace ftxui;

Element render_group_list(const AppState &s) {
    Elements els;
    for (size_t i = 0; i < s.groups.size(); i++) {
        std::string label = s.groups[i].name;
        bool sel = ((int)i == s.group_index);
        if (s.active_panel == 0 && sel)
            els.push_back(theme_accent(text("> " + label) | bold | inverted));
        else if (s.active_panel == 1 && sel)
            els.push_back(theme_fg(text("  " + label) | bold));
        else
            els.push_back(theme_fg(text("  " + label)));
    }
    return theme_bg(vbox(std::move(els)) | yframe | size(WIDTH, EQUAL, 20) | border);
}
