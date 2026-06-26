#include "ui/layout_engine.h"
#include "infra/log.h"
#include <yaml.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ftxui;

LayoutEngine::LayoutEngine() = default;

void LayoutEngine::register_component(const std::string &name, ComponentFn fn) {
    registry_[name] = std::move(fn);
}

/* ── YAML parser ────────────────────────────────────── */
/* Parses a very constrained subset:
     layout:
       type: "vertical"
       flex: 1
       children:
         - component: "status_bar"
           height: 1
         - type: "horizontal"
           ...
*/
static std::unique_ptr<LayoutNode> parse_node(yaml_parser_t *parser);

static std::unique_ptr<LayoutNode> parse_children(yaml_parser_t *parser) {
    auto node = std::make_unique<LayoutNode>();
    node->type = "root";
    
    yaml_event_t event;
    while (yaml_parser_parse(parser, &event)) {
        if (event.type == YAML_SEQUENCE_END_EVENT) {
            yaml_event_delete(&event);
            break;
        }
        if (event.type == YAML_MAPPING_START_EVENT) {
            yaml_event_delete(&event);
            auto child = parse_node(parser);
            if (child) node->children.push_back(std::move(child));
        } else {
            yaml_event_delete(&event);
        }
    }
    return node;
}

static std::unique_ptr<LayoutNode> parse_node(yaml_parser_t *parser) {
    auto node = std::make_unique<LayoutNode>();
    std::string current_key;

    yaml_event_t event;
    while (yaml_parser_parse(parser, &event)) {
        if (event.type == YAML_MAPPING_END_EVENT) {
            yaml_event_delete(&event);
            return node;
        }
        if (event.type == YAML_SCALAR_EVENT) {
            const char *val = (const char*)event.data.scalar.value;
            if (!val) { yaml_event_delete(&event); continue; }

            if (current_key.empty()) {
                current_key = val;
            } else if (current_key == "type") {
                node->type = val;
                current_key.clear();
            } else if (current_key == "component") {
                node->type = val;  /* reuse type field */
                current_key.clear();
            } else if (current_key == "width") {
                node->width = atoi(val);
                current_key.clear();
            } else if (current_key == "height") {
                node->height = atoi(val);
                current_key.clear();
            } else if (current_key == "flex") {
                node->flex = (float)atof(val);
                current_key.clear();
            } else {
                current_key = val; /* skip unknown */
            }
        }
        if (event.type == YAML_SEQUENCE_START_EVENT) {
            if (current_key == "children") {
                yaml_event_delete(&event);
                node->children = std::move(parse_children(parser)->children);
                current_key.clear();
                continue;
            }
        }
        if (event.type == YAML_MAPPING_START_EVENT && current_key == "children") {
            /* inline children: first child starts immediately */
            current_key.clear();
        }
        yaml_event_delete(&event);
    }
    return node;
}

bool LayoutEngine::load(const std::string &yaml_path) {
    FILE *fp = fopen(yaml_path.c_str(), "rb");
    if (!fp) { LOG_WARN("Cannot open layout: %s", yaml_path.c_str()); return false; }

    yaml_parser_t parser;
    yaml_event_t  event;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, fp);

    /* find "layout:" mapping */
    bool in_layout = false;
    while (yaml_parser_parse(&parser, &event)) {
        if (event.type == YAML_STREAM_END_EVENT) { yaml_event_delete(&event); break; }
        if (event.type == YAML_SCALAR_EVENT) {
            const char *val = (const char*)event.data.scalar.value;
            if (val && strcmp(val, "layout") == 0) {
                in_layout = true;
                yaml_event_delete(&event);
                break;
            }
        }
        yaml_event_delete(&event);
    }

    if (!in_layout) { LOG_WARN("No 'layout:' key in YAML"); yaml_parser_delete(&parser); fclose(fp); return false; }

    /* expect mapping start */
    yaml_parser_parse(&parser, &event);
    if (event.type != YAML_MAPPING_START_EVENT) {
        yaml_event_delete(&event);
        yaml_parser_delete(&parser); fclose(fp);
        return false;
    }
    yaml_event_delete(&event);

    root_ = parse_node(&parser);
    yaml_parser_delete(&parser);
    fclose(fp);

    LOG_INFO("Layout loaded: %s  (type=%s, %zu children)", 
             yaml_path.c_str(), root_->type.c_str(), root_->children.size());
    return true;
}

/* ── Build elements ──────────────────────────────────── */
Element LayoutEngine::build(const AppState &state) const {
    if (!root_) {
        /* fallback: default layout */
        auto it = registry_.find("status_bar");
        auto it2 = registry_.find("group_list");
        auto it3 = registry_.find("song_list");
        Elements children;
        if (it != registry_.end()) children.push_back(it->second(state));
        if (it2 != registry_.end() && it3 != registry_.end())
            children.push_back(hbox({it2->second(state), separator(), it3->second(state)}));
        return vbox(std::move(children));
    }

    Elements els;
    for (auto &child : root_->children)
        els.push_back(build_node(*child, state));
    return vbox(std::move(els));
}

Element LayoutEngine::build_node(const LayoutNode &node, const AppState &state) const {
    Element e;

    if (node.type == "vertical" || node.type == "horizontal") {
        Elements children;
        for (auto &child : node.children)
            children.push_back(build_node(*child, state));

        if (node.type == "vertical")
            e = vbox(std::move(children));
        else
            e = hbox(std::move(children));
    } else {
        /* leaf: named component */
        auto it = registry_.find(node.type);
        if (it != registry_.end())
            e = it->second(state);
        else
            e = text("[unknown: " + node.type + "]") | bold;
    }

    /* apply constraints */
    if (node.width > 0)   e = e | size(WIDTH, EQUAL, node.width);
    if (node.height > 0)  e = e | size(HEIGHT, EQUAL, node.height);
    if (node.flex > 0.0f) e = e | flex;

    return e;
}
