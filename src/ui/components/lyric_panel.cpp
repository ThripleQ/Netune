#include "ui/components/lyric_panel.h"
#include "ui/components/theme_util.h"
#include "ui/state_store.h"
#include "core/lyric.h"
#include <string>
#include <vector>
using namespace ftxui;

/* ── UTF-8 split ──────────────────────────────────── */
static std::vector<std::string> utf8_chars(const std::string &s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int len = 1;
        if      ((c & 0xF0) == 0xF0) len = 4;
        else if ((c & 0xE0) == 0xE0) len = 3;
        else if ((c & 0xC0) == 0xC0) len = 2;
        out.push_back(s.substr(i, (size_t)len));
        i += (size_t)len;
    }
    return out;
}

/* ── Gradual fill line (visual effect, not frame-precise) ── */
static Element fill_line(const std::string &txt, float progress) {
    auto chars = utf8_chars(txt);
    if (chars.empty()) return text("");

    int split = (int)(progress * (float)chars.size());
    if (split < 0) split = 0;
    if (split > (int)chars.size()) split = (int)chars.size();

    Elements els;
    els.push_back(text("  "));  /* alignment prefix */
    for (int i = 0; i < (int)chars.size(); i++) {
        if (i < split)
            els.push_back(text(chars[i]) | bold);
        else
            els.push_back(text(chars[i]) | dim);
    }
    return hbox(std::move(els));
}

/* ── Render lyrics ────────────────────────────────── */
static Element render_lyrics(const Lyrics *ly, int play_time_ms) {
    if (!ly || ly->count == 0)
        return text("  No lyrics") | dim | center | flex;

    int base = lyric_find_line(ly, play_time_ms);
    if (base < 0) base = 0;

    /* Progress within the current line (for visual fill effect) */
    float kprog = 0.0f;
    if (base + 1 < ly->count) {
        int t0 = ly->lines[base].time_ms;
        int t1 = ly->lines[base + 1].time_ms;
        int dt = t1 - t0;
        if (dt > 0)
            kprog = (float)(play_time_ms - t0) / (float)dt;
        if (kprog < 0.0f) kprog = 0.0f;
        if (kprog > 1.0f) kprog = 1.0f;
    }

    /* Window around current line — wide enough to fill panel */
    const int above = 4;
    const int below = 20;
    int start = base - above;
    if (start < 0) start = 0;
    int end = base + below;
    if (end > ly->count) end = ly->count;

    /* Top padding: ensure current line sits ~above+1 from top */
    Elements items;
    int top_pad = (base - start < above) ? (above - (base - start)) : 0;
    for (int i = 0; i < top_pad; i++)
        items.push_back(text(""));

    for (int i = start; i < end; i++) {
        std::string raw = ly->lines[i].text ? ly->lines[i].text : "";
        if (i == base) {
            items.push_back(fill_line(raw, kprog));
        } else if (i == base + 1 || i == base - 1) {
            items.push_back(theme_fg(text("  " + raw)));
        } else {
            items.push_back(theme_fg(text("  " + raw)) | dim);
        }
    }

    items.push_back(filler());
    return vbox(std::move(items));
}

/* ── Cover placeholder ────────────────────────────── */
static Element render_cover() {
    return vbox({
        text("") | bold,
        text("  [ Cover ]") | dim | center,
        text("") | bold,
    }) | center | flex;
}

Element render_lyric_panel(const AppState &s) {
    int ms = s.current_time_sec * 1000;
    return theme_bg(hbox({
        render_cover(),
        render_lyrics(s.lyrics, ms),
    }));
}
