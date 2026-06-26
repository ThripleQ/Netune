#include "ui/theme.h"
#include "infra/log.h"
#include <yaml.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

ThemeColor theme_color_from_hex(const std::string &hex) {
    ThemeColor c;
    if (hex.empty() || hex[0] != '#') return c;
    unsigned long val = strtoul(hex.c_str() + 1, nullptr, 16);
    c.r = (uint8_t)((val >> 16) & 0xFF);
    c.g = (uint8_t)((val >> 8) & 0xFF);
    c.b = (uint8_t)(val & 0xFF);
    c.has_color = true;
    return c;
}

ThemeManager& ThemeManager::instance() {
    static ThemeManager mgr;
    return mgr;
}

bool ThemeManager::load(const std::string &yaml_path) {
    FILE *fp = fopen(yaml_path.c_str(), "rb");
    if (!fp) {
        LOG_WARN("Cannot open theme: %s", yaml_path.c_str());
        return false;
    }

    yaml_parser_t parser;
    yaml_event_t  event;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, fp);

    bool in_colors = false;
    std::string hex_bg, hex_fg, hex_accent;
    std::string current_field;

    while (yaml_parser_parse(&parser, &event)) {
        if (event.type == YAML_STREAM_END_EVENT) { yaml_event_delete(&event); break; }
        if (event.type == YAML_SCALAR_EVENT) {
            const char *val = (const char*)event.data.scalar.value;
            if (!val) { yaml_event_delete(&event); continue; }

            if (strcmp(val, "colors") == 0) { in_colors = true; current_field.clear(); }
            else if (in_colors) {
                if (current_field.empty()) {
                    current_field = val;  /* "bg", "fg", "accent" */
                } else {
                    /* value for current_field */
                    if (current_field == "bg")      hex_bg = val;
                    else if (current_field == "fg") hex_fg = val;
                    else if (current_field == "accent") hex_accent = val;
                    current_field.clear();
                }
            } else if (strcmp(val, "name") == 0) {
                /* next scalar is the name */
                yaml_event_delete(&event);
                yaml_parser_parse(&parser, &event);
                if (event.type == YAML_SCALAR_EVENT)
                    theme_.name = (const char*)event.data.scalar.value;
            }
        }
        if (event.type == YAML_MAPPING_END_EVENT) in_colors = false;
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);
    fclose(fp);

    if (!hex_bg.empty())     theme_.bg = theme_color_from_hex(hex_bg);
    if (!hex_fg.empty())     theme_.fg = theme_color_from_hex(hex_fg);
    if (!hex_accent.empty()) theme_.accent = theme_color_from_hex(hex_accent);

    LOG_INFO("Theme loaded: '%s'  bg=%s fg=%s accent=%s",
             theme_.name.c_str(), hex_bg.c_str(), hex_fg.c_str(), hex_accent.c_str());
    return true;
}
