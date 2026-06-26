#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include "ui/state_store.h"
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

/* ── Layout node (tree) ────────────────────────────── */
struct LayoutNode {
    std::string type;        /* "vertical", "horizontal", or component name */
    int width = 0;           /* 0 = auto/unset */
    int height = 0;
    float flex = 0.0f;       /* 0 = no flex */
    std::vector<std::unique_ptr<LayoutNode>> children;
};

/* ── Layout engine ────────────────────────────────────
   Loads a YAML layout config, then builds FTXUI Elements
   by calling registered component render functions.
   ────────────────────────────────────────────────── */
class LayoutEngine {
public:
    using ComponentFn = std::function<ftxui::Element(const AppState&)>;

    LayoutEngine();
    
    /* Register a named component (e.g. "group_list", "status_bar") */
    void register_component(const std::string &name, ComponentFn fn);

    /* Load YAML layout file */
    bool load(const std::string &yaml_path);

    /* Build the full UI element tree */
    ftxui::Element build(const AppState &state) const;

private:
    ftxui::Element build_node(const LayoutNode &node, const AppState &state) const;

    std::unique_ptr<LayoutNode> root_;
    std::unordered_map<std::string, ComponentFn> registry_;
};
