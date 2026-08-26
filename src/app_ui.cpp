#include "app_ui.h"
#include "app_bootstrap.h"
/* FTXUI v6.0.0 headers rely on these but don't include them explicitly */
#include <mutex>
#include <condition_variable>
#include <thread>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/loop.hpp>
#include <signal.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>   /* mkdir */
#define PATH_SEP "/"
#define PATH_SEP_CHR '/'
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>      /* _mkdir */
#define PATH_SEP "\\"
#define PATH_SEP_CHR '\\'
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cstring>
#include "compat/utf8.h"   /* UTF-8 aware getenv/fopen/access/mkdir for Windows */
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <chrono>

extern "C" {
#include "infra/log.h"
#include "infra/config_paths.h"
#include "infra/config.h"
#include "infra/defaults.h"
#include "infra/thread_pool.h"
#include "core/event_bus.h"
#include "core/playback_coordinator.h"
#include "core/music_source_manager.h"
#include "core/music_source.h"
#include "core/audio_output_mgr.h"
#include "core/search_manager.h"
#include "core/cache_manager.h"
#include "core/term_gfx.h"
#include "stb_image.h"
#include "plugins/music_sources/local/local_source.h"
#include "plugins/music_sources/netease/netease_source.h"
#include "plugins/music_sources/netease/netease_ext.h"
#include "plugins/music_sources/netease/netease_quality.h"
}

#include "ui/state_store.h"
#include "ui/download_queue.h"
#include "ui/keybindings.h"
#include "ui/mpris.h"
#include "ui/ui_util.h"
#include "ui/components/top_bar.h"
#include "ui/components/status_bar.h"
#include "ui/components/group_list.h"
#include "ui/components/song_list.h"
#include "ui/components/help_screen.h"
#include "ui/components/login_screen.h"
#include "ui/components/action_sheet.h"
#include "ui/components/lyric_panel.h"

extern "C" {
#include "core/lyric.h"
#include "plugins/lyrics/lrc/lrc_parser.h"
}

/* Netease 特化接口（netease_ext.h）。netease_ext() 返回编译期 static
   const 表，声明即初始化，无 NULL 窗口；app_ui.cpp 通过它调用网易云
   能力，不直接依赖 netease_api.h 实现。 */
static const NeteaseExt *g_ne = netease_ext();
#include "ui/theme.h"
#include "ui/components/theme_util.h"
#include "ui/layout_engine.h"

using namespace ftxui;

/* ── Global keybinding manager ──────────────────────── */
static KeybindingManager g_keybindings;
static threadpool_t *g_thread_pool = NULL;

static volatile bool g_running = true;

/* ── Event → key name ────────────────────────────────
   Maps a raw FTXUI event to the canonical key name used by
   the keybinding table ("up", "ctrl+n", "f5", "j", ...).
   Returns empty string for events that are NOT keys:
   mouse, cursor updates, IME text (multi-byte UTF-8), etc.
   ──────────────────────────────────────────────────── */
static std::string event_to_key_name(const ftxui::Event &event) {
    /* Alt combos: terminal sends ESC + char (2 bytes) */
    if (event.input().size() == 2 &&
        (unsigned char)event.input()[0] == 0x1b &&
        (unsigned char)event.input()[1] >= 32 &&
        (unsigned char)event.input()[1] != 127) {
        return "alt+" + event.input().substr(1, 1);
    }
    /* ── Special events (C0 control chars) ──
       ftxui routes every byte < 0x20 to Event::Special; Ctrl+/ is
       US (0x1f) or NUL (0x00) depending on the terminal. */
    if (event.input().size() == 1) {
        unsigned char c = (unsigned char)event.input()[0];
        if (c == 0x1f || c == 0x00)
            return "ctrl+/";
        if (c == 0x1c)
            return "ctrl+\\";
        if (c == 0x1d)
            return "ctrl+]";
        if (c == 0x1e)
            return "ctrl+^";
    }
    /* ── Navigation / editing keys ── */
    if (event == ftxui::Event::ArrowUp)        return "up";
    if (event == ftxui::Event::ArrowDown)      return "down";
    if (event == ftxui::Event::ArrowLeft)      return "left";
    if (event == ftxui::Event::ArrowRight)     return "right";
    if (event == ftxui::Event::ArrowUpCtrl)    return "ctrl+up";
    if (event == ftxui::Event::ArrowDownCtrl)  return "ctrl+down";
    if (event == ftxui::Event::ArrowLeftCtrl)  return "ctrl+left";
    if (event == ftxui::Event::ArrowRightCtrl) return "ctrl+right";
    if (event == ftxui::Event::Backspace)      return "backspace";
    if (event == ftxui::Event::Delete)         return "delete";
    if (event == ftxui::Event::Return)         return "enter";
    if (event == ftxui::Event::Escape)         return "escape";
    if (event == ftxui::Event::Tab)            return "tab";
    if (event == ftxui::Event::TabReverse)     return "shift+tab";
    if (event == ftxui::Event::Insert)         return "insert";
    if (event == ftxui::Event::Home)           return "home";
    if (event == ftxui::Event::End)            return "end";
    if (event == ftxui::Event::PageUp)         return "pageup";
    if (event == ftxui::Event::PageDown)       return "pagedown";

    /* ── Function keys ── */
    if (event == ftxui::Event::F1)  return "f1";
    if (event == ftxui::Event::F2)  return "f2";
    if (event == ftxui::Event::F3)  return "f3";
    if (event == ftxui::Event::F4)  return "f4";
    if (event == ftxui::Event::F5)  return "f5";
    if (event == ftxui::Event::F6)  return "f6";
    if (event == ftxui::Event::F7)  return "f7";
    if (event == ftxui::Event::F8)  return "f8";
    if (event == ftxui::Event::F9)  return "f9";
    if (event == ftxui::Event::F10) return "f10";
    if (event == ftxui::Event::F11) return "f11";
    if (event == ftxui::Event::F12) return "f12";

    /* ── Letters with modifiers (a-z) ──
       FTXUI defines a/A/CtrlA/AltA/CtrlAltA per letter. Build the
       canonical name via a lookup table to keep this readable. */
    struct KeyTriple {
        const ftxui::Event *ctrl, *alt, *ctrlalt;
        char letter;
    };
    /* (macro-free table — each row: Ctrl event, Alt event, CtrlAlt event, letter) */
    const KeyTriple keys[] = {
        {&ftxui::Event::CtrlA, &ftxui::Event::AltA, &ftxui::Event::CtrlAltA, 'a'},
        {&ftxui::Event::CtrlB, &ftxui::Event::AltB, &ftxui::Event::CtrlAltB, 'b'},
        {&ftxui::Event::CtrlC, &ftxui::Event::AltC, &ftxui::Event::CtrlAltC, 'c'},
        {&ftxui::Event::CtrlD, &ftxui::Event::AltD, &ftxui::Event::CtrlAltD, 'd'},
        {&ftxui::Event::CtrlE, &ftxui::Event::AltE, &ftxui::Event::CtrlAltE, 'e'},
        {&ftxui::Event::CtrlF, &ftxui::Event::AltF, &ftxui::Event::CtrlAltF, 'f'},
        {&ftxui::Event::CtrlG, &ftxui::Event::AltG, &ftxui::Event::CtrlAltG, 'g'},
        {&ftxui::Event::CtrlH, &ftxui::Event::AltH, &ftxui::Event::CtrlAltH, 'h'},
        {&ftxui::Event::CtrlI, &ftxui::Event::AltI, &ftxui::Event::CtrlAltI, 'i'},
        {&ftxui::Event::CtrlJ, &ftxui::Event::AltJ, &ftxui::Event::CtrlAltJ, 'j'},
        {&ftxui::Event::CtrlK, &ftxui::Event::AltK, &ftxui::Event::CtrlAltK, 'k'},
        {&ftxui::Event::CtrlL, &ftxui::Event::AltL, &ftxui::Event::CtrlAltL, 'l'},
        {&ftxui::Event::CtrlM, &ftxui::Event::AltM, &ftxui::Event::CtrlAltM, 'm'},
        {&ftxui::Event::CtrlN, &ftxui::Event::AltN, &ftxui::Event::CtrlAltN, 'n'},
        {&ftxui::Event::CtrlO, &ftxui::Event::AltO, &ftxui::Event::CtrlAltO, 'o'},
        {&ftxui::Event::CtrlP, &ftxui::Event::AltP, &ftxui::Event::CtrlAltP, 'p'},
        {&ftxui::Event::CtrlQ, &ftxui::Event::AltQ, &ftxui::Event::CtrlAltQ, 'q'},
        {&ftxui::Event::CtrlR, &ftxui::Event::AltR, &ftxui::Event::CtrlAltR, 'r'},
        {&ftxui::Event::CtrlS, &ftxui::Event::AltS, &ftxui::Event::CtrlAltS, 's'},
        {&ftxui::Event::CtrlT, &ftxui::Event::AltT, &ftxui::Event::CtrlAltT, 't'},
        {&ftxui::Event::CtrlU, &ftxui::Event::AltU, &ftxui::Event::CtrlAltU, 'u'},
        {&ftxui::Event::CtrlV, &ftxui::Event::AltV, &ftxui::Event::CtrlAltV, 'v'},
        {&ftxui::Event::CtrlW, &ftxui::Event::AltW, &ftxui::Event::CtrlAltW, 'w'},
        {&ftxui::Event::CtrlX, &ftxui::Event::AltX, &ftxui::Event::CtrlAltX, 'x'},
        {&ftxui::Event::CtrlY, &ftxui::Event::AltY, &ftxui::Event::CtrlAltY, 'y'},
        {&ftxui::Event::CtrlZ, &ftxui::Event::AltZ, &ftxui::Event::CtrlAltZ, 'z'},
    };
    for (const auto &k : keys) {
        if (event == *k.ctrl)    return std::string("ctrl+") + k.letter;
        if (event == *k.alt)     return std::string("alt+") + k.letter;
        if (event == *k.ctrlalt) return std::string("ctrl+alt+") + k.letter;
    }

    /* ── Plain printable character (single byte ASCII) ── */
    if (event.is_character()) {
        const std::string &ch = event.character();
        if (ch.size() == 1) {
            unsigned char c = (unsigned char)ch[0];
            /* Ctrl+/ — terminals encode it differently: US (0x1f) or
               NUL (0x00); map every candidate to one canonical key */
            if (c == 0x1f || c == 0x00)
                return "ctrl+/";
        }
        if (ch.size() == 1 && (unsigned char)ch[0] >= 32 && (unsigned char)ch[0] < 127)
            return (ch == " ") ? "space" : ch;
        /* multi-byte UTF-8 (IME text): not a key */
        return "";
    }

    /* everything else (mouse, cursor, custom) is not a key */
    return "";
}

/* Seek accumulation, resize baseline, seek target/last-event time and
   the top-search nav flag all live in StateStore now (see AppState:
   seek_accum / seek_target / seek_last_ms / last_resize_* /
   top_search_pushed) — they were file-level globals before. */

static void on_signal(int sig) { (void)sig; g_running = false; }

/* ── Login state ────────────────────────────────────── */
static std::string g_login_unikey;
static int         g_login_poll_tick = 0;

/* High-resolution QR image (kitty graphics path). Fetched in the
   background when the QR screen opens; the render loop places it over
   the login layout when ready. */
static CoverData    g_login_qr = {0};
static volatile int g_login_qr_ready = 0;
static uint64_t     g_login_qr_stamp = 0;

/* Minimal base64 decoder (the qr-image command emits base64 PNG) */
static std::vector<uint8_t> b64_decode(const char *s) {
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (; *s; ++s) {
        if (*s == '=') break;
        unsigned char c = (unsigned char)*s;
        int d;
        if (c >= 'A' && c <= 'Z')      d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+')             d = 62;
        else if (c == '/')             d = 63;
        else continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

/* Generate QR code text using netease-cli's built-in qr-render */
static std::string gen_qr(const char *url) {
    char *qr = g_ne->qr_render(url);
    if (!qr) return "";
    std::string s(qr);
    free(qr);
    return s;
}

/* Start the QR login flow using netease-cli */
static void start_login(void) {
    StateStore::instance().set_login_state(1, "Contacting server...", "");
    char unikey[128] = {0};
    char qr_url[512] = {0};
    if (g_ne->qr_key(unikey, sizeof(unikey), qr_url, sizeof(qr_url)) == 0
        && unikey[0]) {
        g_login_unikey = unikey;
        g_login_poll_tick = 0;
        LOG_INFO("LOGIN KEY: url_len=%zu url='%.120s'", strlen(qr_url), qr_url);
        std::string qr = gen_qr(qr_url);
        int qr_nl = 0;
        for (size_t i = 0; i < qr.size(); i++)
            if (qr[i] == '\n') qr_nl++;
        LOG_INFO("LOGIN QR: text_len=%zu lines=%d head='%.40s' mid='%.40s' tail='%.40s'",
                 qr.size(), qr_nl, qr.c_str(),
                 qr.size() > 200 ? qr.c_str() + 200 : "",
                 qr.size() > 80 ? qr.c_str() + qr.size() - 80 : "");
        StateStore::instance().set_login_state(2,
            "Scan with Netease Music App", qr);
        /* unikey is valid for ~2 minutes; count down from there */
        StateStore::instance().set_login_deadline((long)time(NULL) + 120);
        StateStore::instance().set_login_net_error(0);

        /* Fetch the high-resolution QR image in the background for
           terminals with kitty graphics support. */
        g_login_qr_ready = 0;
        StateStore::instance().set_qr_gfx_ready(0);
        if (g_login_qr.pixels) {
            free(g_login_qr.pixels);
            g_login_qr.pixels = NULL;
        }
        std::string qr_url_copy = qr_url;
        std::thread([](std::string url) {
            char *b64 = g_ne->qr_image(url.c_str());
            if (!b64) return;
            std::vector<uint8_t> png = b64_decode(b64);
            free(b64);
            if (png.empty()) return;
            int w = 0, h = 0, ch = 0;
            uint8_t *px = stbi_load_from_memory(png.data(), (int)png.size(),
                                                &w, &h, &ch, 3);
            if (!px) return;
            g_login_qr.pixels = px;
            g_login_qr.width = w;
            g_login_qr.height = h;
            g_login_qr.channels = 3;
            g_login_qr.stamp = ++g_login_qr_stamp;
            g_login_qr_ready = 1;
            StateStore::instance().set_qr_gfx_ready(1);
        }, qr_url_copy).detach();
    } else {
        StateStore::instance().set_login_state(-1,
            "Failed to get QR code", "");
    }
}

/* Update netease menu after successful login */
static void update_login_menu(void) {
    const auto &cur = StateStore::instance().state();
    if (cur.netease_menu.empty()) return;
    const char *name = g_ne->account_name();
    if (!name) name = "Logged in";
    auto menu = cur.netease_menu;
    std::string label = "";
    label += name;
    menu[0].name = label;
    menu[0].type = 300;  /* account page entry */
    StateStore::instance().set_netease_menu(menu);
}

#include <map>

static void ev_search_done(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    LOG_INFO("Search done — results in playlist");
    StateStore::instance().set_loading(false);
    StateStore::instance().set_search_active(false);
    StateStore::instance().set_search_query("");
    StateStore::instance().set_search_scope(0);
    StateStore::instance().set_active_panel(1);
}

/* ── Netease search: async, results go to playlist ─── */
#define NS_CACHE_MAX 32
static std::map<std::string, std::vector<SongInfo>> g_ns_cache;

/* Free all SongInfo strings in a cache vector (for eviction / shutdown) */
static void ns_cache_vec_free(std::vector<SongInfo> &v) {
    for (auto &s : v) song_info_free(&s);
    v.clear();
}

struct LoadedSongs { SongInfo *songs; int count; };

/* Payload for EV_SEARCH_LOADED: search results posted back to the main
   thread. query + songs are heap-allocated; the main-thread callback
   takes ownership (writes g_ns_cache there — never from the worker). */
struct SearchLoadResult {
    char      *query;
    SongInfo  *songs;
    int        count;
};

static void do_netease_search(const char *query, bool push_nav) {
    if (!query || !query[0]) return;
    std::string q(query);

    /* Check cache first */
    auto it = g_ns_cache.find(q);
    if (it != g_ns_cache.end()) {
        if (push_nav) StateStore::instance().nav_push();
        StateStore::instance().set_playlist(it->second, 0);
        StateStore::instance().set_active_panel(1);
        StateStore::instance().set_search_active(false);
        StateStore::instance().set_search_query("");
        return;
    }

    if (push_nav) StateStore::instance().nav_push();
    StateStore::instance().set_loading(true);

    std::thread([q]() {
        NSSearchResult nr;
        if (g_ne->search(q.c_str(), 100, 0, &nr) != 0) {
            /* search failed — publish done to clear search+loading */
            event_bus_publish(EV_SEARCH_DONE, NULL, 0);
            return;
        }

        std::vector<SongInfo> vec;
        vec.reserve(nr.count + 16);
        for (int i = 0; i < nr.count; i++) {
            SongInfo si = {};
            si.id       = strdup(nr.songs[i].id);
            si.source   = strdup("netease");
            si.title    = strdup(nr.songs[i].title ? nr.songs[i].title : "");
            si.artist   = strdup(nr.songs[i].artist ? nr.songs[i].artist : "");
            si.album    = strdup(nr.songs[i].album ? nr.songs[i].album : "");
            si.duration_sec = nr.songs[i].dur_ms / 1000;
            si.fee          = nr.songs[i].fee;
            vec.push_back(si);
        }
        g_ne->search_free(&nr);

        /* Append matching playlists so a query finds both songs and
           playlist entries (distinguished by is_playlist). */
        SongInfo *pls = NULL; int pc = 0;
        if (g_ne->search_playlists(q.c_str(), &pls, &pc) == 0 && pc > 0) {
            for (int i = 0; i < pc; i++)
                vec.push_back(pls[i]);
            free(pls);
        }

        /* Post the results to the main thread; g_ns_cache is written there
           (never touched from this worker), so the map needs no locking. */
        int sc = (int)vec.size();
        SearchLoadResult *res = (SearchLoadResult*)malloc(sizeof(SearchLoadResult));
        SongInfo *songs = (SongInfo*)calloc((size_t)(sc > 0 ? sc : 1),
                                            sizeof(SongInfo));
        for (int i = 0; i < sc; i++)
            song_info_copy(&songs[i], &vec[i]);
        res->query = strdup(q.c_str());
        res->songs = songs;
        res->count = sc;
        for (auto &s : vec) song_info_free(&s);   /* payload owns the copies */
        if (event_bus_publish(EV_SEARCH_LOADED, res, sizeof(*res)) != 0) {
            for (int i = 0; i < sc; i++) song_info_free(&songs[i]);
            free(songs);
            free(res->query);
            free(res);
            return;
        }

        /* Signal search done — exit search mode */
        event_bus_publish(EV_SEARCH_DONE, NULL, 0);
    }).detach();
}

/* Free search cache on shutdown */
/* Apply cached netease search results instantly, if present.
   Pushes the nav stack once per search session so Esc can restore
   the pre-search view. Returns true when applied. */
static bool netease_search_apply_cache(const std::string &q) {
    auto &store = StateStore::instance();
    auto it = g_ns_cache.find(q);
    if (it == g_ns_cache.end()) return false;
    if (!store.state().top_search_pushed) {
        store.nav_push_restore_playlist();
        store.set_top_search_pushed(true);
    }
    store.set_playlist(it->second, 0);
    store.set_active_panel(1);
    return true;
}

/* Close the top search row and restore the pre-search view. The box
   QUERY is kept so reopening search shows the last typed term (search
   has memory); only the nav snapshot (list + menu) is restored. */
/* Close the top search row.
   - Filter mode (Ctrl+/): clear the queries so the original list/menu
     comes back in one step.
   - API mode (搜索网易云): keep the query (search memory) and pop the
     nav snapshot to restore the pre-search list. */
static void close_top_search(void) {
    StateStore &st = StateStore::instance();
    if (!st.state().top_search_api) {
        st.set_top_right_query("");
    }
    if (st.state().top_search_pushed) {
        st.nav_pop();
        st.set_top_search_pushed(false);
    }
    st.set_top_search_active(false);
}

/* Clear ALL search state — used when the user leaves the search context
   (activates another menu item / local group): the new list must not be
   filtered by the old query and the search nav push must unwind. */
static void clear_search_state(void) {
    StateStore &st = StateStore::instance();
    st.set_top_right_query("");
    st.set_top_search_active(false);
    st.set_top_search_api(false);
    if (st.state().top_search_pushed) {
        st.nav_pop();
        st.set_top_search_pushed(false);
    }
}

/* After a right-box query edit: apply cached results instantly when the
   list is empty; otherwise fall back to the pre-search view so stale
   results don't linger. */
static void restore_search_view(const std::string &q) {
    if (!StateStore::instance().state().top_search_pushed) return;
    if (!q.empty() && netease_search_apply_cache(q)) return;
    /* cache miss or cleared query — restore the pre-search view */
    NavState snap;
    if (StateStore::instance().nav_peek(snap)) {
        StateStore::instance().set_playlist(snap.playlist, snap.selected_index);
        StateStore::instance().set_active_panel(snap.active_panel);
    }
}

/* ── Rebuild the local-music groups (one per configured dir) ── */
static void refresh_local_groups(void) {
    Config *cfg = config_global();
    std::vector<std::string> scan_dirs;
    int ndirs = cfg ? config_get_array_size(cfg, "music_sources.local.dirs") : 0;
    if (ndirs > 0) {
        for (int i = 0; i < ndirs; i++) {
            char key[64];
            snprintf(key, sizeof(key), "music_sources.local.dirs[%d]", i);
            const char *d = config_get_str(cfg, key, NULL);
            if (d) scan_dirs.push_back(d);
        }
    }
    std::vector<SongGroup> groups;
    /* the app's own downloads folder: its group gets a flag so the UI can
       surface live download progress there */
    const std::string dl_dir =
        std::string(netune_data_root()) + PATH_SEP + "downloads";
    auto is_dl_dir = [&](const std::string &d) {
#ifdef _WIN32
        return _stricmp(d.c_str(), dl_dir.c_str()) == 0;
#else
        return d == dl_dir;
#endif
    };
    if (!scan_dirs.empty()) {
        /* The local source's search matches titles/filenames, not dirs, so
           fetch everything (empty keyword = all songs) and group by the id
           path prefix of each configured dir. */
        SearchResult all;
        memset(&all, 0, sizeof(all));
        int rc = music_source_search("local", "", 0, 0, &all);
        for (auto &dir : scan_dirs) {
            SongGroup g;
            g.is_downloads = is_dl_dir(dir);
            const char *slash_fwd = strrchr(dir.c_str(), '/');
            const char *slash_back = strrchr(dir.c_str(), '\\');
            const char *last_sep = (slash_back && (!slash_fwd || slash_back > slash_fwd))
                                   ? slash_back : slash_fwd;
            g.name = last_sep ? last_sep + 1 : dir.c_str();
            if (g.is_downloads)
                g.name = "DOWNLOADS";  /* the app's own download folder, capitalised */
            if (rc == 0 && all.songs) {
                size_t dlen = dir.size();
                for (int j = 0; j < all.count; j++) {
                    const SongInfo *s = &all.songs[j];
                    if (!s->id || strncmp(s->id, dir.c_str(), dlen) != 0)
                        continue;
                    /* require a separator after the dir prefix */
                    if (s->id[dlen] != '/' && s->id[dlen] != '\\' &&
                        s->id[dlen] != '\0')
                        continue;
                    SongInfo copy = {};
                    song_info_copy(&copy, s);
                    g.songs.push_back(copy);
                }
            }
            if (!g.songs.empty())
                groups.push_back(std::move(g));
        }
        if (all.songs) {
            for (int j = 0; j < all.count; j++)
                song_info_free(&all.songs[j]);
            free(all.songs);
        }
    }
    /* Remember where the user is BEFORE set_groups: it resets
       group_index to -1 (the netease entry), and reading the state back
       afterwards would always see -1. */
    const auto &st0 = StateStore::instance().state();
    int  old_gi     = st0.group_index;
    bool was_local  = st0.music_mode == MusicMode::Local;

    StateStore::instance().set_groups(groups);
    if (!groups.empty()) {
        LOG_INFO("Local groups: %zu groups scanned", groups.size());
    } else {
        LOG_WARN("No music files found");
    }
    /* Restore the user's position in the local view (if we are in it):
       - -1 (netease entry) stays -1
       - a group index stays on that group (re-fills the right panel with
         the freshly scanned songs) unless the group count shrank. */
    if (was_local) {
        const auto &st2 = StateStore::instance().state();
        int n = (int)st2.groups.size();
        int target = -1;
        if (old_gi >= 0 && old_gi < n)
            target = old_gi;
        else if (old_gi >= 0 && n > 0)
            target = 0;
        StateStore::instance().set_group_index(target);
    }
}

/* ── Activate a netease menu item: load its content into the right panel ── */
static void activate_netease_menu_item(int idx) {
    /* Leaving the search context: the new content must not inherit the
       query filter or the search nav push. */
    clear_search_state();
    const auto &cur = StateStore::instance().state();
    if (idx < 0 || idx >= (int)cur.netease_menu.size()) return;
    int type = cur.netease_menu[idx].type;
    const std::string &pl_id = cur.netease_menu[idx].id;


    if (type == -1) {
        /* Back to main netease menu: clear nav stack
           so Esc doesn't go back to playlist folder */
        StateStore::instance().clear_nav_stack();
        /* Clear first so set_music_mode reinitializes */
        StateStore::instance().set_netease_menu({});
        StateStore::instance().set_music_mode(MusicMode::Local);
        StateStore::instance().set_music_mode(MusicMode::Netease);
    } else if (type == 200) {
        start_login();
    } else if (type == 100) {
        /* netease "search" menu entry — clear the right list and any
           stale query so the box switches to API search mode instead of
           filtering the previous menu's songs, then focus the box */
        StateStore::instance().set_playlist({}, 0);
        StateStore::instance().set_top_right_query("");
        StateStore::instance().set_top_search_active(true);
        StateStore::instance().set_top_search_api(true);
    } else if (type == 300) {
        /* account page: submenu of the logged-in user.
           nav_push so Esc (and the snapshot) can restore the main menu */
        StateStore::instance().nav_push();
        StateStore::instance().set_netease_menu({
            {"<< \u8FD4\u56DE", -1, ""},
            {"\u6211\u7684\u6B4C\u5355", 301, ""},
            {"\u6211\u559C\u6B22\u7684\u97F3\u4E50", 302, ""},
            {"\u5237\u65B0\u767B\u5F55", 303, ""},
            {"\u9000\u51FA\u767B\u5F55", 304, ""},
            {"\u65B0\u5EFA\u6B4C\u5355", 305, ""},
        });
        StateStore::instance().set_netease_selected(0);
    } else if (type == 301) {
        /* my playlists */
        StateStore::instance().nav_push();
        StateStore::instance().set_loading(true);
        std::thread([]() {
            SongInfo *pl = NULL; int pc = 0;
            int ret = g_ne->playlists(false, &pl, &pc);
            LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
            if (ret == 0 && pc > 0) {
                for (int i = 0; i < pc; i++) pl[i].mine = 1;
                ld->songs = pl; ld->count = pc;
            } else {
                ld->songs = NULL; ld->count = 0;
                free(pl);
            }
            if (event_bus_publish(EV_PLAYLIST_LIST_LOADED, ld, sizeof(*ld)) != 0) {
                if (ld->songs) {
                    for (int i = 0; i < ld->count; i++)
                        song_info_free(&ld->songs[i]);
                    free(ld->songs);
                }
                free(ld);
            }
        }).detach();
    } else if (type == 302 || type == 4) {
        /* liked songs (account page 302 / main menu 4) */
        StateStore::instance().nav_push();
        StateStore::instance().set_loading(true);
        std::thread([]() {
            SongInfo *ms = NULL; int mc = 0;
            int ret = g_ne->liked_songs(&ms, &mc);
            LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
            if (ret == 0 && mc > 0) {
                ld->songs = ms; ld->count = mc;
            } else {
                ld->songs = NULL; ld->count = 0;
                free(ms);
            }
            if (event_bus_publish(EV_PLAYLIST_LOADED, ld, sizeof(*ld)) != 0) {
                if (ld->songs) {
                    for (int i = 0; i < ld->count; i++)
                        song_info_free(&ld->songs[i]);
                    free(ld->songs);
                }
                free(ld);
            }
        }).detach();
    } else if (type == 303) {
        /* refresh login */
        std::thread([]() {
            int ok = g_ne->login_refresh();
            if (ok == 0) {
                const char *name = g_ne->account_name();
                StateStore::instance().set_login_state(3,
                    name ? name : "Logged in", "");
            } else {
                StateStore::instance().set_login_state(-1,
                    "Refresh login failed", "");
            }
        }).detach();
    } else if (type == 304) {
        /* logout: drop cookies, rebuild the default (logged-out) menu
           without leaving Netease mode (no flicker to Local) */
        g_ne->logout();
        StateStore::instance().clear_nav_stack();
        StateStore::instance().set_netease_menu({});
        StateStore::instance().set_music_mode(MusicMode::Netease);
    } else if (type == 305) {
        /* create playlist: open the action sheet in text-input mode */
        StateStore::instance().set_action_sheet(true, 0);
        StateStore::instance().set_action_sheet_menu(2);
        StateStore::instance().set_action_sheet_ctx("create");
        StateStore::instance().set_action_sheet_input("");
    } else if (type == 306) {
        /* my purchased: submenu of purchased singles + digital albums */
        if (!g_ne->is_logged_in()) { start_login(); }
        else {
            StateStore::instance().nav_push();
            StateStore::instance().set_netease_menu({
                {"<< \u8FD4\u56DE", -1, ""},
                {"\u5DF2\u8D2D\u5355\u66F2", 307, ""},
                {"\u5DF2\u8D2D\u6570\u5B57\u4E13\u8F91", 308, ""},
            });
            StateStore::instance().set_netease_selected(0);
        }
    } else if (type == 307) {
        /* purchased single tracks */
        StateStore::instance().nav_push();
        StateStore::instance().set_loading(true);
        std::thread([]() {
            SongInfo *songs = NULL; int sc = 0;
            int ret = g_ne->purchased_songs(&songs, &sc);
            LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
            if (ret == 0 && sc > 0) {
                ld->songs = songs; ld->count = sc;
            } else {
                ld->songs = NULL; ld->count = 0;
                free(songs);
            }
            if (event_bus_publish(EV_PLAYLIST_LOADED, ld, sizeof(*ld)) != 0) {
                if (ld->songs) {
                    for (int i = 0; i < ld->count; i++)
                        song_info_free(&ld->songs[i]);
                    free(ld->songs);
                }
                free(ld);
            }
        }).detach();
    } else if (type == 308) {
        /* purchased digital albums */
        StateStore::instance().nav_push();
        StateStore::instance().set_loading(true);
        std::thread([]() {
            SongInfo *songs = NULL; int sc = 0;
            int ret = g_ne->purchased_albums(&songs, &sc);
            LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
            if (ret == 0 && sc > 0) {
                ld->songs = songs; ld->count = sc;
            } else {
                ld->songs = NULL; ld->count = 0;
                free(songs);
            }
            if (event_bus_publish(EV_PLAYLIST_LIST_LOADED, ld, sizeof(*ld)) != 0) {
                if (ld->songs) {
                    for (int i = 0; i < ld->count; i++)
                        song_info_free(&ld->songs[i]);
                    free(ld->songs);
                }
                free(ld);
            }
        }).detach();
    } else if (!pl_id.empty()) {
        /* entering a playlist's songs from a playlist list — push with
           playlist restore so Esc lands back on the playlist list */
        StateStore::instance().nav_push_restore_playlist();
        StateStore::instance().set_current_playlist_id(pl_id);
        StateStore::instance().set_detail_playlist_mine(
            !cur.playlist.empty() && cur.playlist[cur.selected_index].mine == 1);
        StateStore::instance().set_loading(true);
        /* purchased digital albums are is_playlist=1 rows whose Enter opens
           the album's tracks (album API) rather than a playlist's songs */
        bool is_purchased_album =
            !cur.playlist.empty() && cur.playlist[cur.selected_index].aux_label &&
            strcmp(cur.playlist[cur.selected_index].aux_label, "已购专辑") == 0;
        std::string _pl_id = pl_id;
        std::thread([_pl_id, is_purchased_album]() {
            SongInfo *songs = NULL; int sc = 0;
            int ret = is_purchased_album
                ? g_ne->album_songs(_pl_id.c_str(), &songs, &sc)
                : g_ne->playlist_songs(_pl_id.c_str(), &songs, &sc);
            LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
            if (ret == 0 && sc > 0) {
                ld->songs = songs; ld->count = sc;
            } else {
                ld->songs = NULL; ld->count = 0;
                free(songs); /* safe: NULL or alloc */
            }
            if (event_bus_publish(EV_PLAYLIST_LOADED, ld, sizeof(*ld)) != 0) {
                /* shutdown — clean up data ourselves */
                if (ld->songs) {
                    for (int i = 0; i < ld->count; i++)
                        song_info_free(&ld->songs[i]);
                    free(ld->songs);
                }
                free(ld);
            }
        }).detach();

    } else if (type == 0 || type == 1 || type == 5 ||
               type == 6 || type == 7) {
        /* 每日推荐 (0, songs) / 推荐歌单 (1, playlists) / 排行榜 (5)
           最近播放 (6, songs) / 每日歌单 (7, playlists) */
        if (!g_ne->is_logged_in() &&
            (type == 0 || type == 6 || type == 7)) {
            start_login();  /* these need a session */
        } else {
            StateStore::instance().nav_push();
            StateStore::instance().set_loading(true);
            std::thread([type]() {
                SongInfo *songs = NULL; int sc = 0;
                int ret = -1;
                if (type == 5)            ret = g_ne->toplist(&songs, &sc);
                else if (type == 6)       ret = g_ne->recent_songs(&songs, &sc);
                else if (type == 7)       ret = g_ne->daily_playlists(&songs, &sc);
                else                      ret = g_ne->menu_songs(type, 30, &songs, &sc);
                LOG_INFO("MENU SONGS: type=%d ret=%d count=%d", type, ret, sc);
                LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
                if (ret == 0 && sc > 0) {
                    ld->songs = songs; ld->count = sc;
                } else {
                    ld->songs = NULL; ld->count = 0;
                    free(songs);
                }
                EventType ev = (type == 1 || type == 5 || type == 7)
                    ? EV_PLAYLIST_LIST_LOADED : EV_PLAYLIST_LOADED;
                if (event_bus_publish(ev, ld, sizeof(*ld)) != 0) {
                    if (ld->songs) {
                        for (int i = 0; i < ld->count; i++)
                            song_info_free(&ld->songs[i]);
                        free(ld->songs);
                    }
                    free(ld);
                }
            }).detach();
        }
    } else if (type == 2 || type == 3) {
        if (!g_ne->is_logged_in()) {
            start_login();
        } else {
            StateStore::instance().nav_push();
            StateStore::instance().set_loading(true);
            std::thread([type]() {
                SongInfo *pl = NULL; int pc = 0;
                int ret = g_ne->playlists(type == 3, &pl, &pc);
                LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
                if (ret == 0 && pc > 0) {
                    ld->songs = pl; ld->count = pc;
                } else {
                    ld->songs = NULL; ld->count = 0;
                    free(pl);
                }
                if (event_bus_publish(EV_PLAYLIST_LIST_LOADED, ld, sizeof(*ld)) != 0) {
                    if (ld->songs) {
                        for (int i = 0; i < ld->count; i++)
                            song_info_free(&ld->songs[i]);
                        free(ld->songs);
                    }
                    free(ld);
                }
            }).detach();
        }
    } else if (type == 4) {
        /* 我喜欢的音乐 (liked songs) */
        if (!g_ne->is_logged_in()) {
            start_login();
        } else {
            StateStore::instance().nav_push();
            StateStore::instance().set_loading(true);
            std::thread([]() {
                SongInfo *songs = NULL; int sc = 0;
                int ret = g_ne->liked_songs(&songs, &sc);
                LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
                if (ret == 0 && sc > 0) {
                    ld->songs = songs; ld->count = sc;
                } else {
                    ld->songs = NULL; ld->count = 0;
                    free(songs);
                }
                if (event_bus_publish(EV_PLAYLIST_LOADED, ld, sizeof(*ld)) != 0) {
                    if (ld->songs) {
                        for (int i = 0; i < ld->count; i++)
                            song_info_free(&ld->songs[i]);
                        free(ld->songs);
                    }
                    free(ld);
                }
            }).detach();
        }
    }
}


void netease_search_cache_free(void) {
    for (auto &kv : g_ns_cache)
        ns_cache_vec_free(kv.second);
    g_ns_cache.clear();
}

/* ── Search event → StateStore bridge ─────────────── */
static void ev_search_start(const BusEvent *ev, void *data) {
    (void)data;
    StateStore::instance().set_search_active(true);
    StateStore::instance().set_search_query(
        ev->data ? (const char*)ev->data : "");
}

static void ev_search_result(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    const SearchResult *sr = search_manager_results();
    if (sr && sr->count > 0) {
        /* Build vector with proper deep copies */
        std::vector<SongInfo> vec;
        vec.reserve(sr->count);
        for (int i = 0; i < sr->count; i++) {
            SongInfo copy = {};
            song_info_copy(&copy, &sr->songs[i]);
            vec.push_back(copy);
        }
        StateStore::instance().set_search_results(vec, sr->total);
        /* clean up the copies after set_search_results re-copies them */
        for (auto &v : vec) song_info_free(&v);
    }
}

static void ev_search_error(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    StateStore::instance().set_search_active(false);
    StateStore::instance().set_search_query("");
}

/* ── Lyrics ───────────────────────────────────────── */
/* EV_LYRIC_LOADED payload: { Lyrics*, song_id* } — both heap-allocated.
   Ownership: the UI callback compares song_id with the current song; on
   match it takes the Lyrics (replacing the old one) and frees song_id,
   on a stale song it frees both. */
struct LyricLoadResult {
    Lyrics *lyrics;   /* may be NULL on failure */
    char   *song_id;
};

/* Netease lyric fetch runs in a background thread — the CLI does a network
   round-trip (hundreds of ms); doing it synchronously on the UI thread
   froze the interface on every track switch. */
static void lyric_load_worker(void *arg) {
    char *song_id = (char*)arg;
    char *lyric_buf = NULL;
    Lyrics *ly = NULL;
    if (g_ne->lyric(song_id, &lyric_buf) == 0 && lyric_buf) {
        ly = lyric_parse(lyric_buf);
        free(lyric_buf);
    }
    LyricLoadResult res = { ly, song_id };
    event_bus_publish(EV_LYRIC_LOADED, &res, sizeof(res));
}

/* In-flight marker (UI thread only): song whose lyrics are being fetched.
   Prevents duplicate spawns from EV_TRACK_CHANGED + EV_PLAYBACK_START
   firing for the same song. */
static void ev_lyric_loaded(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size != sizeof(LyricLoadResult)) return;
    const LyricLoadResult *res = (const LyricLoadResult*)ev->data;
    auto &store = StateStore::instance();
    const SongInfo &cur = store.state().current_song;
    bool match = res->song_id && cur.id &&
                 strcmp(res->song_id, cur.id) == 0;

    store.set_lyric_pending_id("");

    if (match && res->lyrics) {
        Lyrics *old = store.state().lyrics;
        store.set_lyrics(res->lyrics);
        if (old) lyric_free(old);
        LOG_INFO("Lyrics loaded from Netease (%d lines)", res->lyrics->count);
    } else {
        if (res->lyrics) lyric_free(res->lyrics);
    }
    free(res->song_id);
}

/* ── Start cover download (shows spinner, clears old cover) ──── */
/* ── Cover downloaded in background thread ─────────── */
static void cover_download_worker(void *arg) {
    char *url = (char*)arg;
    CoverData cd = {NULL, 0, 0, 0, 0};
    if (url && cover_load(url, &cd) == 0)
        event_bus_publish(EV_COVER_LOADED, &cd, sizeof(CoverData));
    free(url);
}

/* ── Start cover download (shows spinner, clears old cover) ──── */
static void start_cover_download(const char *url) {
    if (!url || !url[0]) return;
    CoverData empty = {NULL, 0, 0, 0, 0};
    StateStore::instance().set_cover(empty);
    StateStore::instance().set_cover_loading(true);
    char *u = strdup(url);
    if (u) threadpool_submit(g_thread_pool, cover_download_worker, u);
}

/* ── Spectrum update event (from playback thread) ──── */
static void ev_spectrum(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data && ev->data_size == sizeof(float) * SPECTRUM_BANDS) {
        StateStore::instance().set_spectrum((const float*)ev->data);
    }
}

/* ── Cover loaded event (from background thread) ───── */
static void ev_cover_loaded(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data && ev->data_size == sizeof(CoverData)) {
        CoverData *cd = (CoverData*)ev->data;
        StateStore::instance().set_cover(*cd);
    }
}

/* ── Event bus → StateStore bridge ────────────────── */
static void ev_progress(const BusEvent *ev, void *data) {
    (void)data;
    auto &store = StateStore::instance();
    if (store.state().playback_state == PlaybackState::Stopped)
        return;
    if (ev->data_size == sizeof(int[4])) {
        int *p = (int*)ev->data;
        double prog = (p[1] > 0) ? (double)p[0] / p[1] : 0.0;
        int tot = (p[2] > 0 && p[1] > 0) ? p[1] / p[2] : 0;
        int cur_ms = (p[2] > 0) ? (int)((long long)p[0] * 1000LL / p[2]) : 0;
        /* 4th slot: stream bitrate (bits/sec) — refresh the status bar tag */
        store.set_current_bitrate(p[3]);
        /* Clear seek target once progress is within 1s of the target (seek executed) */
        int target = store.state().seek_target;
        int seek_dist = cur_ms / 1000 - target;
        if (target >= 0 && seek_dist >= -1 && seek_dist <= 1) {
            store.set_seek_target(-1);
            store.set_seek_target_progress(0.0f);
            /* Progress caught up — fall through to update */
            target = -1;
        }
        if (target < 0) {
            /* No pending seek: normal progress update.
               Skip when seek is pending but playback hasn't caught up
               yet — prevents stale EV_PROGRESS_UPDATE (sent before the
               playback thread processed CMD_SEEK) from overwriting the
               already-correct progress set by consume_seek(). */
            store.set_progress_ms(prog, cur_ms, tot);
        }
    }
}
/* forward decl — defined below */
static void load_lyrics_for_current_song(void);

static void ev_playback_start(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    StateStore::instance().set_playback_state(PlaybackState::Playing);
    load_lyrics_for_current_song();
}
static void ev_playback_pause(const BusEvent *ev, void *data) {
    (void)ev; (void)data; StateStore::instance().set_playback_state(PlaybackState::Paused);
}
static void ev_playback_resume(const BusEvent *ev, void *data) {
    (void)ev; (void)data; StateStore::instance().set_playback_state(PlaybackState::Playing);
}
static void ev_playback_stop(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    StateStore::instance().set_playback_state(PlaybackState::Stopped);
    StateStore::instance().set_current_song(SongInfo{});
    StateStore::instance().set_current_quality("");
    StateStore::instance().set_current_bitrate(0);
    StateStore::instance().set_progress(0, 0, 0);
    StateStore::instance().set_lyric_mode(false);
}
static void ev_playback_error(const BusEvent *ev, void *data) {
    (void)ev; (void)data; LOG_WARN("Playback error"); StateStore::instance().set_playback_state(PlaybackState::Stopped); StateStore::instance().set_progress(0,0,0);
}
/* ── Play the queue song at `idx` (shared by finish/next/prev) ──
   Returns true if playback was started. Assumes idx is a valid
   queue index already produced by StateStore::queue_step(). */
static bool play_queue_index(int idx) {
    auto &store = StateStore::instance();
    const SongInfo *song = store.queue_current();
    if (!song || !song->id || !song->id[0]) {
        store.set_playback_state(PlaybackState::Stopped);
        return false;
    }
    store.set_selected_index(idx);
    store.set_current_song(*song);
    event_bus_publish(EV_TRACK_CHANGED, NULL, 0);
    event_bus_publish(EV_PLAYBACK_START, (void*)song->id, strlen(song->id) + 1);
    return true;
}

/* ── Start playback from the visible playlist (space/enter/search) ──
   Snapshots the current playlist into the queue, then plays the
   song at `idx`. Returns true if playback started. */
/* ── Filtered navigation ───────────────────────────────
   While a filter query is active the list/menu shows only matching
   items, so up/down must skip non-matching entries — otherwise the
   highlight lands on invisible rows. Returns the next matching index
   (wrapping) or -1 when nothing matches. */
static int next_match(const std::vector<SongInfo> &list, int from, int step,
                      const std::string &q) {
    int n = (int)list.size();
    if (n <= 0) return -1;
    std::string lq = q;
    std::transform(lq.begin(), lq.end(), lq.begin(), ::tolower);
    for (int k = 1; k <= n; k++) {
        int idx = (from + step * k) % n;
        if (idx < 0) idx += n;
        const auto &s = list[idx];
        std::string h;
        if (s.title) h += s.title;
        if (s.artist) h += std::string(" ") + s.artist;
        std::transform(h.begin(), h.end(), h.begin(), ::tolower);
        if (h.find(lq) != std::string::npos)
            return idx;
    }
    return -1;
}

static bool play_from_playlist(int idx) {
    auto &store = StateStore::instance();
    const auto &cur = store.state();
    if (idx < 0 || idx >= (int)cur.playlist.size()) return false;
    const auto &sel = cur.playlist[idx];
    /* Enter on the currently-playing song must NOT restart it from 0 —
       that read as "播放开始后回到开头继续播放" when the play command
       re-arrived (e.g. re-pressing Enter on the same row while it was
       already opening/playing). Same track + Playing → keep playing;
       same track + Paused → resume. */
    if (cur.current_song.id && sel.id &&
        strcmp(cur.current_song.id, sel.id) == 0) {
        store.set_selected_index(idx);
        if (cur.playback_state == PlaybackState::Paused)
            event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
        else if (cur.playback_state == PlaybackState::Playing)
            return true;  /* already playing: no-op, don't restart */
    }
    store.queue_snapshot();
    store.set_current_song(sel);
    event_bus_publish(EV_TRACK_CHANGED, NULL, 0);
    event_bus_publish(EV_PLAYBACK_START, (void*)(sel.id ? sel.id : ""),
                      strlen(sel.id ? sel.id : "") + 1);
    return true;
}

static void ev_playback_finish(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    auto &store = StateStore::instance();
    int next = store.queue_advance();
    if (next < 0) {
        /* end of queue: stop, but keep the queue so prev/play still work */
        store.set_playback_state(PlaybackState::Stopped);
        return;
    }
    const SongInfo *song = store.queue_current();
    LOG_INFO("ADV: next=%d path=%s", next,
             (song && song->id) ? song->id : "null");
    play_queue_index(next);
}

/* A track could not be played (no copyright / no stream URL) — auto
   advance to the next track so listening isn't interrupted. */
static void ev_playback_skip(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    auto &store = StateStore::instance();
    int next = store.queue_advance();
    if (next < 0) {
        store.set_playback_state(PlaybackState::Stopped);
        return;
    }
    const SongInfo *song = store.queue_current();
    LOG_WARN("SKIP unplayable track, next=%d id=%s", next,
             (song && song->id) ? song->id : "null");
    play_queue_index(next);
}

/* ── Play/Pause toggle (shared by space key and MPRIS PlayPause) ── */
static void toggle_playback(void) {
    const AppState &cur = StateStore::instance().state();
    if (cur.playback_state == PlaybackState::Playing)
        event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
    else if (cur.playback_state == PlaybackState::Paused)
        event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
    else if (cur.playback_state == PlaybackState::Stopped)
        play_from_playlist(cur.selected_index);
}

/* ── Seek to an absolute position in seconds (shared by arrows/MPRIS) ──
   Note: unlike consume_seek (keyboard hold-debounce), this executes the
   seek unconditionally — single-shot MPRIS calls must not be skipped. */
static void do_seek_to(int target) {
    auto &store = StateStore::instance();
    const AppState &cur = store.state();
    if (cur.playback_state == PlaybackState::Stopped || cur.total_time_sec <= 0)
        return;
    if (target > cur.total_time_sec) target = cur.total_time_sec;
    if (target < 0) target = 0;
    event_bus_publish(EV_BUFFERING_UPDATE, &target, sizeof(target));
    store.set_seek_target(target);
    store.set_seek_target_progress((float)target / cur.total_time_sec);
    /* Immediately sync progress so the gauge doesn't fall back
       to the old s.progress value when seek_target_progress is
       0.0f (target=0 → 0/total → 0.0f, and > 0.0f is false). */
    store.set_progress((double)target / cur.total_time_sec, target,
                       cur.total_time_sec);
}

/* ── MPRIS external control (PlayPause/Stop/Next/Prev/Seek) ──
   Commands arrive from the D-Bus thread via EV_MPRIS_COMMAND;
   handled here on the main thread so StateStore access is safe.
   payload: int[2] = { MprisCommand, arg } */
static void ev_mpris_command(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size != 2 * sizeof(int)) return;
    const int *cmd = (const int*)ev->data;
    auto &store = StateStore::instance();

    switch (cmd[0]) {
    case MPRIS_CMD_PLAYPAUSE:
        toggle_playback();
        break;

    case MPRIS_CMD_STOP:
        store.queue_clear();
        event_bus_publish(EV_PLAYBACK_STOP, NULL, 0);
        break;

    case MPRIS_CMD_NEXT: {
        int next = store.queue_next();
        if (next >= 0) play_queue_index(next);
        break;
    }

    case MPRIS_CMD_PREV: {
        int prev = store.queue_prev();
        if (prev >= 0) play_queue_index(prev);
        break;
    }

    case MPRIS_CMD_SEEK:
        do_seek_to(cmd[1]);
        break;

    default:
        LOG_WARN("Unknown MPRIS command %d", cmd[0]);
        break;
    }
}

/* ── Volume / Mute / Playlist event handlers ──────── */

/* ── Async playlist loading helper ───────────────────── */
/* Netease search results returned to the main thread. The g_ns_cache map
   is written HERE (main thread only) — the search worker never touches it,
   so no locking is needed. */
static void ev_search_loaded(const BusEvent *ev, void *data) {
    (void)data;
    auto *res = (SearchLoadResult*)ev->data;
    if (!res) return;
    /* Note: ev->data is freed by event_bus_poll, do NOT free(res) */
    std::string q = res->query ? res->query : "";
    free(res->query);
    if (res->count <= 0 || !res->songs) {
        free(res->songs);
        StateStore::instance().set_loading(false);
        return;
    }
    std::vector<SongInfo> vec;
    vec.reserve((size_t)res->count);
    for (int i = 0; i < res->count; i++) {
        SongInfo copy = {};
        song_info_copy(&copy, &res->songs[i]);
        vec.push_back(copy);
        song_info_free(&res->songs[i]);
    }
    free(res->songs);

    StateStore::instance().set_playlist(vec, 0);
    StateStore::instance().set_active_panel(1);
    StateStore::instance().set_loading(false);

    /* write the result into the search cache (LRU, cap 32) — after
       set_playlist so vec can be moved in without an extra deep copy */
    if (!q.empty()) {
        if (g_ns_cache.size() >= NS_CACHE_MAX) {
            auto oldest = g_ns_cache.begin();
            ns_cache_vec_free(oldest->second);
            g_ns_cache.erase(oldest);
        }
        g_ns_cache[q] = std::move(vec);
    }
}

static void ev_playlist_loaded(const BusEvent *ev, void *data) {
    (void)data;
    auto *ld = (LoadedSongs*)ev->data;
    if (!ld || ld->count <= 0) {
        StateStore::instance().set_loading(false);
        return;
    }
    std::vector<SongInfo> vec;
    vec.reserve(ld->count);
    for (int i = 0; i < ld->count; i++) {
        SongInfo copy = {};
        song_info_copy(&copy, &ld->songs[i]);
        vec.push_back(copy);
        song_info_free(&ld->songs[i]);
    }
    free(ld->songs);
    /* Note: ev->data is freed by event_bus_poll, do NOT free(ld) */
    StateStore::instance().set_playlist(vec, 0);
    StateStore::instance().set_active_panel(1);
    StateStore::instance().set_loading(false);
}

/* ── Menu songs loaded (daily recommend etc.) ────────── */
static void ev_menu_loaded(const BusEvent *ev, void *data) {
    (void)data;
    auto *ld = (LoadedSongs*)ev->data;
    if (!ld || ld->count <= 0) {
        StateStore::instance().set_loading(false);
        return;
    }
    std::vector<SongInfo> vec;
    vec.reserve(ld->count);
    for (int i = 0; i < ld->count; i++) {
        SongInfo copy = {};
        song_info_copy(&copy, &ld->songs[i]);
        vec.push_back(copy);
        song_info_free(&ld->songs[i]);
    }
    free(ld->songs);
    /* Note: ev->data is freed by event_bus_poll, do NOT free(ld) */
    StateStore::instance().set_playlist(vec, 0);
    StateStore::instance().set_active_panel(1);
    StateStore::instance().set_loading(false);
}

/* ── Playlist list loaded (netease folders) ──────────── */
static void ev_playlist_list_loaded(const BusEvent *ev, void *data) {
    (void)data;
    StateStore::instance().set_loading(false);
    if (!ev->data) return;
    auto *ld = (LoadedSongs*)ev->data;
    LOG_INFO("PLAYLIST LIST LOADED: count=%d", ld->count);
    if (ld->count <= 0 || !ld->songs) {
        /* ev->data freed by event_bus_poll, do NOT free(ld) */
        return;
    }
    /* Playlist list goes to the RIGHT panel (like a song list), each
       item tagged as a playlist via aux_label so Enter opens the
       playlist instead of playing it. */
    std::vector<SongInfo> vec;
    vec.reserve(ld->count);
    for (int i = 0; i < ld->count; i++) {
        SongInfo copy = {};
        song_info_copy(&copy, &ld->songs[i]);
        vec.push_back(copy);
        song_info_free(&ld->songs[i]);
    }
    free(ld->songs);
    /* Note: ev->data is freed by event_bus_poll, do NOT free(ld) */
    StateStore::instance().set_playlist(vec, 0);
    StateStore::instance().set_active_panel(1);
}

/* ── Download queue update (backend → StateStore mirror) ──
   Runs on the main thread via event_bus_poll. Snapshots the active tasks
   into StateStore (the UI never reads across threads into the queue).
   A payload is the completion notice (C-string); ev->data is freed by
   event_bus_poll, do NOT free it here. */
static void ev_download_update(const BusEvent *ev, void *data) {
    (void)data;
    StateStore::instance().set_downloads(DownloadQueue::instance().active());
    if (ev->data && ev->data_size == sizeof(DlNoticePayload)) {
        const DlNoticePayload *pl = (const DlNoticePayload*)ev->data;
        StateStore::instance().set_app_notice(pl->msg, pl->kind);
    } else if (ev->data && ev->data_size > 0) {
        /* legacy bare-string payload (no colour kind) */
        StateStore::instance().set_app_notice((const char*)ev->data, 0);
    }
}

static void ev_volume_changed(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int)) {
        int vol = *(int*)ev->data;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        audio_output_set_volume(vol);
        StateStore::instance().set_volume(vol);
        /* persist so the setting survives restarts */
        Config *gcfg = config_global();
        if (gcfg) {
            config_set_int(gcfg, "audio.volume", vol);
            config_save(gcfg);
        }
    }
}

static void ev_mute_changed(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int)) {
        int muted = *(int*)ev->data;
        StateStore::instance().set_muted(muted != 0);
        int target = muted ? 0 : StateStore::instance().state().volume;
        if (target <= 0) target = 80;
        audio_output_set_volume(target);
    }
}

static void load_lyrics_for_current_song(void) {
    auto &store = StateStore::instance();
    const SongInfo &song = store.state().current_song;

    /* free old lyrics */
    Lyrics *old = store.state().lyrics;
    if (old) { lyric_free(old); store.set_lyrics(NULL); }

    if (!song.id) return;

    if (song.source && strcmp(song.source, "local") == 0) {
        /* local song: look for .lrc sidecar file (fast local IO, stays
           synchronous so lyrics are ready immediately) */
        size_t len = strlen(song.id);
        char *lrc_path = (char*)malloc(len + 5);
        if (!lrc_path) return;
        memcpy(lrc_path, song.id, len);
        lrc_path[len] = '\0';
        char *dot = strrchr(lrc_path, '.');
        if (dot) *dot = '\0';
        strcat(lrc_path, ".lrc");

        Lyrics *ly = lrc_load_file(lrc_path);
        free(lrc_path);
        if (ly) {
            store.set_lyrics(ly);
            LOG_INFO("Lyrics loaded from .lrc (%d lines)", ly->count);
        }
        return;
    } else if (song.source && strcmp(song.source, "netease") == 0) {
        /* Netease: fetch via netease-cli in a background thread */
        if (!store.state().lyric_pending_id.empty() &&
            store.state().lyric_pending_id == song.id)
            return;  /* already loading this song's lyrics */
        char *dup = strdup(song.id);
        if (!dup || !g_thread_pool) { free(dup); return; }
        threadpool_submit(g_thread_pool, lyric_load_worker, dup);
        store.set_lyric_pending_id(song.id);
        /* Cover is now loaded lazily when entering lyric mode */
        return;
    }
}

/* Refresh the current-quality mirror in StateStore from the override/global
   resolution, then, if the now-playing netease track's effective quality
   actually changed and it is exactly `song_id` (or song_id is null = any),
   tell the playback thread to reopen it at the new quality and resume at
   the current position (in-place quality switch). */
static void quality_changed_refresh(const char *song_id) {
    auto &st = StateStore::instance();
    const AppState &s = st.state();

    /* recompute the effective quality for the now-playing netease track */
    std::string new_eff;
    if (s.music_mode == MusicMode::Netease &&
        s.current_song.id && s.current_song.id[0]) {
        char lvl[64] = {0};
        const char *eff = nullptr;
        if (nq_override_get(s.current_song.id, lvl, sizeof lvl) == 0 && lvl[0])
            eff = lvl;
        else
            eff = nq_global_level();
        if (eff && eff[0]) new_eff = eff;
    }
    bool changed = (new_eff != s.current_quality);
    st.set_current_quality(new_eff);

    /* reopen the playing track in place only if its quality really changed */
    bool playing = (s.playback_state == PlaybackState::Playing ||
                    s.playback_state == PlaybackState::Paused);
    if (!playing || !changed || s.music_mode != MusicMode::Netease ||
        !s.current_song.id || !s.current_song.id[0])
        return;
    if (song_id && song_id[0] && strcmp(song_id, s.current_song.id) != 0)
        return;  /* the changed song isn't the one playing */
    PlaybackReloadCmd rc = {};
    snprintf(rc.id, sizeof rc.id, "%s", s.current_song.id);
    rc.seek_sec = s.current_time_sec;
    event_bus_publish(EV_PLAYBACK_RELOAD, &rc, sizeof(rc));
}

static void ev_track_changed(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    load_lyrics_for_current_song();
    /* Resolve the effective quality once at track-change time (per-song
       override > global default), so status_bar just reads the cached tag
       instead of hitting nq_override_get (which reads a JSON file) on every
       render frame. Uses the cheap override/global lookup only — the full
       nq_resolve_level (with network probing) happens on the async play
       path, so we must not block the main thread on a probe here. */
    auto &stq = StateStore::instance();
    const SongInfo &cs = stq.state().current_song;
    if (stq.state().music_mode == MusicMode::Netease &&
        cs.id && cs.id[0]) {
        char lvl[64] = {0};
        if (nq_override_get(cs.id, lvl, sizeof lvl) == 0 && lvl[0])
            stq.set_current_quality(lvl);
        else {
            const char *g = nq_global_level();
            stq.set_current_quality(g ? g : "");
        }
    } else {
        stq.set_current_quality("");
    }
    /* Always clear the old cover — a track without artwork must not leave
       the previous cover lingering (the raw-image overlay would keep
       showing it); a new cover_url triggers a fresh download. */
    CoverData empty = {NULL, 0, 0, 0, 0};
    StateStore::instance().set_cover(empty);
    StateStore::instance().set_cover_loading(false);
    if (StateStore::instance().state().lyric_mode)
        start_cover_download(StateStore::instance().state().current_song.cover_url);
}

static void ev_playlist_changed(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size == sizeof(int)) {
        int mode = *(int*)ev->data;
        StateStore::instance().set_loop_mode((LoopMode)mode);
    }
}
/* ───────────────────────────────────────────────────── */

/* ── XDG path helpers ────────────────────────────── */
/* ── XDG / data-root path helpers ────────────────────
   Moved to infra/config_paths.c so netune and netune-config share
   ONE implementation (netune_config.cpp used to carry a drifted
   copy that lacked the Windows APPDATA branch). */

int run_app(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Netune v2.0.0 — Terminal music player\nUsage: %s [--config] [config.json]\n", argv[0]);
        return 0;
    }

    /* One-time startup wiring (log / config / cache / event bus /
       thread pool / download queue) lives in app_bootstrap. */
    Config *cfg = app_bootstrap(&g_thread_pool);

    /* Default templates for themes/keybindings/layouts are created by
       the bootstrap — nothing else to do here. */

        event_bus_subscribe(EV_PROGRESS_UPDATE,   ev_progress, NULL);
    event_bus_subscribe(EV_PLAYBACK_START,    ev_playback_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE,    ev_playback_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME,   ev_playback_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP,     ev_playback_stop, NULL);
    event_bus_subscribe(EV_PLAYBACK_FINISH,   ev_playback_finish, NULL);
    event_bus_subscribe(EV_PLAYBACK_SKIP,     ev_playback_skip, NULL);
    event_bus_subscribe(EV_MPRIS_COMMAND,     ev_mpris_command, NULL);
    event_bus_subscribe(EV_LOCAL_REFRESH,     [](const BusEvent *, void *) {
        refresh_local_groups();
    }, NULL);
    event_bus_subscribe(EV_DOWNLOAD_UPDATE,   ev_download_update, NULL);
        event_bus_subscribe(EV_PLAYLIST_LOADED, ev_playlist_loaded, NULL);
        event_bus_subscribe(EV_SEARCH_LOADED,   ev_search_loaded,   NULL);
    event_bus_subscribe(EV_MENU_LOADED, ev_menu_loaded, NULL);
    event_bus_subscribe(EV_PLAYLIST_LIST_LOADED, ev_playlist_list_loaded, NULL);
    event_bus_subscribe(EV_PLAYBACK_ERROR,    ev_playback_error, NULL);
    event_bus_subscribe(EV_VOLUME_CHANGED,    ev_volume_changed, NULL);
    event_bus_subscribe(EV_MUTE_CHANGED,      ev_mute_changed, NULL);
    event_bus_subscribe(EV_PLAYLIST_CHANGED,  ev_playlist_changed, NULL);
    event_bus_subscribe(EV_TRACK_CHANGED,  ev_track_changed, NULL);
    /* search events — StateStore bridge */
    event_bus_subscribe(EV_SEARCH_START, ev_search_start, NULL);
    event_bus_subscribe(EV_SEARCH_RESULT, ev_search_result, NULL);
    event_bus_subscribe(EV_SEARCH_ERROR, ev_search_error, NULL);
    event_bus_subscribe(EV_SEARCH_DONE, ev_search_done, NULL);
    event_bus_subscribe(EV_COVER_LOADED, ev_cover_loaded, NULL);
    event_bus_subscribe(EV_LYRIC_LOADED, ev_lyric_loaded, NULL);
    event_bus_subscribe(EV_SPECTRUM_UPDATE, ev_spectrum, NULL);

    if (cfg) {
        int vol = config_get_int(cfg, "audio.volume", -1);
        if (vol >= 0 && vol <= 100) {
            StateStore::instance().set_volume(vol);
            /* seed the audio manager so +/- work before first playback */
            audio_output_set_initial_volume(vol);
        }
        int loop = config_get_int(cfg, "playback.loop_mode", 0);
        if (loop >= 0 && loop <= 3) {
            StateStore::instance().set_loop_mode((LoopMode)loop);
        }
    }

    /* load keybindings — always from data/keybindings/<name>.yaml */
    const char *kb_name = config_get_str(cfg, "ui.keybindings", NULL);
    const char *kb_path;
    static char kb_buf[2048];
    if (kb_name && strcmp(kb_name, "default") != 0
#ifndef _WIN32
        && kb_name[0] == '/') {
#else
        && ((kb_name[0] && kb_name[1] == ':') || kb_name[0] == '/' || kb_name[0] == '\\')) {
#endif
        kb_path = kb_name;  /* absolute path: use as-is */
    } else {
        const char *name = (kb_name && strcmp(kb_name, "default") != 0) ? kb_name : "default";
        snprintf(kb_buf, sizeof(kb_buf), "%s" PATH_SEP "keybindings" PATH_SEP "%s.yaml",
                 netune_data_root(), name);
        kb_buf[sizeof(kb_buf) - 1] = '\0';
        kb_path = kb_buf;
    }
    g_keybindings.load(kb_path);

    /* load theme — use ThemeManager::resolve_path for proper name→path resolution */
    const char *t_name = config_get_str(cfg, "ui.theme", nullptr);
    std::string t_path = ThemeManager::resolve_path(t_name ? t_name : "default");
    ThemeManager::instance().load(t_path);

    /* layout engine — always load from data/layouts/<name>.yaml */
    LayoutEngine layout_engine;
    layout_engine.register_component("top_bar", render_top_bar);
    layout_engine.register_component("status_bar", render_status_bar);
    layout_engine.register_component("group_list", render_group_list);
    layout_engine.register_component("song_list", render_song_list);
    const char *l_name = config_get_str(cfg, "ui.layout", NULL);
    char l_buf[2048];
    const char *l_path;
    if (l_name && strcmp(l_name, "default") != 0
#ifndef _WIN32
        && l_name[0] == '/' && access_utf8(l_name, F_OK) == 0) {
#else
        && (((l_name[0] && l_name[1] == ':') || l_name[0] == '/' || l_name[0] == '\\')
            && access_utf8(l_name, F_OK) == 0)) {
#endif
        l_path = l_name;  /* absolute path: use as-is */
    } else {
        const char *name = (l_name && strcmp(l_name, "default") != 0) ? l_name : "default";
        snprintf(l_buf, sizeof(l_buf), "%s" PATH_SEP "layouts" PATH_SEP "%s.yaml",
                 netune_data_root(), name);
        l_buf[sizeof(l_buf) - 1] = '\0';
        l_path = l_buf;
    }
    layout_engine.load(l_path);

    music_source_manager_init();
    local_source_register();
    netease_source_register();

    /* Open on the Netease homepage by default: rebuild its default menu
       now that netease_init has resolved the account name. */
    StateStore::instance().set_music_mode(MusicMode::Netease);

    refresh_local_groups();

    playback_coordinator_init();

#ifndef _WIN32
    mpris_init();
#endif

    event_bus_publish(EV_APP_STARTUP, NULL, 0);
    event_bus_poll();

    signal(SIGINT, on_signal);
#ifndef _WIN32
    signal(SIGTERM, on_signal);
#endif

    auto screen = ScreenInteractive::Fullscreen();

    std::atomic<bool> timer_active{true};
    std::thread refresh_timer([&]() {
        while (timer_active.load()) {
            auto &st = StateStore::instance().state();
            /* Animation frame interval. Windows keeps 33ms (~30fps):
               conhost renders each frame slowly, so 60fps only burned CPU
               without visible smoothness. Linux/macOS keep the original
               16ms (60fps) — terminal rendering there is fast enough.
               The action sheet (Ctrl+X) also gets fast frames so its
               status query / state flips feel responsive. */
#ifdef _WIN32
            int ms = (st.playback_state == PlaybackState::Playing || st.loading || st.cover_loading || st.action_sheet_open || st.action_sheet_quality_probing || !st.downloads.empty())
                      ? 33 : 200;
#else
            /* marquee-scrolling selected row needs smooth frames even
               when idle — otherwise the 130ms/column scroll jumps 1-2
               columns per 200ms idle frame (visible stutter) */
            bool marquee_row = (st.active_panel == 1 &&
                                !st.playlist.empty() &&
                                !st.top_search_active);
            int ms = (st.playback_state == PlaybackState::Playing || st.loading || st.cover_loading || st.action_sheet_open || st.action_sheet_quality_probing || !st.downloads.empty() || marquee_row)
                      ? 16 : 200;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            screen.RequestAnimationFrame();
        }
    });

    auto &state = StateStore::instance();

    auto consume_seek = [&]() {
        auto now = std::chrono::steady_clock::now();
        auto now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()).count();
        const AppState &st = state.state();
        if (st.seek_accum == 0) return;
        /* Hold-to-accumulate: don't consume while the key is still being
           held. Terminals report only key-press (no key-release), so we
           infer "still holding" from how recently a seek key fired. The
           threshold must exceed the OS key-repeat initial delay (~250-400ms
           on Linux), otherwise the single press at the start of a hold gets
           consumed 150ms in, causing a visible "accumulate → sudden seek →
           accumulate again" stutter. Repeat presses land ~30-60ms apart, so
           500ms cleanly separates "holding" from "released". */
        if (now_ms - st.seek_last_ms < 500)
            return;
        if (st.playback_state != PlaybackState::Stopped && st.total_time_sec > 0) {
            int target = st.current_time_sec + st.seek_accum;
            if (target > st.total_time_sec) target = st.total_time_sec;
            if (target < 0) target = 0;
            /* Skip seek if already at target — prevents rapid
               seek-to-0 loops when holding past the start */
            if (target != st.current_time_sec) {
                event_bus_publish(EV_BUFFERING_UPDATE, &target, sizeof(target));
                state.set_seek_target(target);
                state.set_seek_target_progress((float)target / st.total_time_sec);
                /* Immediately sync progress so the gauge doesn't fall back
                   to the old s.progress value when seek_target_progress is
                   0.0f (target=0 → 0/total → 0.0f, and > 0.0f is false). */
                state.set_progress((double)target / st.total_time_sec, target, st.total_time_sec);
            }
        }
        state.set_seek_accum(0);
        state.set_seek_indicator(0);
    };

    auto component = Renderer([&]() -> Element {
        event_bus_poll();
#ifndef _WIN32
        mpris_sync(&state.state());
#endif
        consume_seek();
        const AppState &s = state.state();

        state.set_song_panel_width(Terminal::Size().dimx - 22);
        state.set_screen_height(Terminal::Size().dimy);
        state.set_top_row_width(Terminal::Size().dimx);

        /* Login polling: wall-clock based (the render loop runs at
           200ms/frame when idle — tick counting would slow polling to
           tens of seconds). Poll every 1s, auto-close the success
           overlay 10s after login (or sooner when data loading ends). */
        static auto last_poll = std::chrono::steady_clock::now();
        static auto login_done_at = std::chrono::steady_clock::now();
        if (s.login_state == 3) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - login_done_at).count() >= 10000)
                StateStore::instance().set_login_state(0, "", "");
        }
        if (s.login_state == 2 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_poll).count() >=
                /* poll every 2s before the scan; once scanned (802)
                   speed up to 1s for a snappy confirm */
                (s.login_status.find("Scanned") != std::string::npos ? 1000 : 2000)) {
            last_poll = std::chrono::steady_clock::now();
            /* A window too small to render the QR also pauses polling:
               the "network error" banner is driven by poll failures and
               must not be triggered by a small terminal. */
            int qneed_w = 0, qneed_h = 0;
            bool qr_fits =
                qr_min_dims(s.login_qr, &qneed_w, &qneed_h) &&
                s.top_row_width >= qneed_w && s.screen_height >= qneed_h;
            if (qr_fits) {
            int rc = g_ne->qr_poll(g_login_unikey.c_str());
            LOG_INFO("LOGIN POLL: rc=%d", rc);
            if (rc == 0) {
                /* 803: authorized, login successful */
                login_done_at = std::chrono::steady_clock::now();
                StateStore::instance().set_login_state(3,
                    g_ne->account_name() ? g_ne->account_name() : "Logged in", "");
                update_login_menu();
                /* Data-loading phase: warm up the user's playlists in
                   the background; the success overlay closes when this
                   finishes (or after the 10s timeout above). */
                std::thread([]() {
                    SongInfo *pl = NULL; int pc = 0;
                    g_ne->playlists(false, &pl, &pc);
                    if (pl) {
                        for (int i = 0; i < pc; i++)
                            song_info_free(&pl[i]);
                        free(pl);
                    }
                    StateStore::instance().set_login_state(0, "", "");
                }).detach();
            } else if (rc == 2) {
                /* 800: expired — restart */
                g_login_unikey.clear();
                start_login();
            } else if (rc == 3) {
                /* 802: scanned, waiting for phone confirm */
                StateStore::instance().set_login_state(2,
                    "Scanned. Confirm in Netease Music App...", s.login_qr);
                StateStore::instance().set_login_net_error(0);
            } else {
                /* -1: poll failed (network/cli) — show it, keep retrying */
                StateStore::instance().set_login_net_error(1);
            }
            }  /* if (qr_fits) */
        }

        Element main;
        if (s.lyric_mode) {
            main = vbox(Elements{
                render_top_bar(s),
                render_lyric_panel(s) | flex,
                render_spectrum_bar(s),
                render_status_bar(s),
            });
        } else {
            /* normal layout */
            main = vbox(Elements{
                layout_engine.build(s) | flex,
            });
        }

        if (s.login_state != 0) {
            /* Full-page login screen */
            return render_login_screen(s);
        }
        if (s.show_help) {
            /* full-page help screen, reflects user keybindings */
            return render_help_screen(s, g_keybindings);
        }
        if (s.action_sheet_open) {
            /* Ctrl+X action sheet overlays the normal UI */
            return dbox({main, render_action_sheet(s)});
        }

        /* transient notice (download results etc.): one line pinned to the
           bottom of the screen, auto-cleared after 4 seconds. Colour follows
           the notice kind: success / error / warning theme slots, else dim. */
        if (!s.app_notice.empty() &&
            (long)time(NULL) - s.app_notice_ts < 4) {
            Element txt = text(" " + s.app_notice + " ") | bold;
            if (s.app_notice_kind == 1)      txt = theme_success(txt);
            else if (s.app_notice_kind == 2) txt = theme_error(txt);
            else if (s.app_notice_kind == 3) txt = theme_warning(txt);
            else                             txt = txt | dim;
            auto notice = vbox({filler(),
                hbox({filler(), txt, filler()})}) | size(HEIGHT, LESS_THAN, 2);
            return dbox({main, notice});
        }

        return main;
    });

    component |= CatchEvent([&](ftxui::Event event) -> bool {
        /* ── Terminal resize signal ──
           ftxui routes SIGWINCH as Event::Special({0}) — the SAME bytes
           as a real Ctrl+/ on some terminals. Disambiguate by checking
           whether the terminal size actually changed since the last
           known size (baseline seeded at startup); if so this is a
           resize, swallow it (a real Ctrl+/ leaves the size untouched). */
        {
            if (event.input().size() == 1 &&
                (unsigned char)event.input()[0] == 0x00) {
                auto tsz = Terminal::Size();
                if (tsz.dimx != state.state().last_resize_w ||
                    tsz.dimy != state.state().last_resize_h) {
                    state.set_last_resize(tsz.dimx, tsz.dimy);
                    return true;  /* swallow the resize signal */
                }
            }
        }

        /* ── Action sheet (Ctrl+X) modal state machine ── */
        {
            const AppState &as = state.state();
            if (as.action_sheet_open) {
                std::string k;
                if (event.is_character()) k = event.character();
                else k = event_to_key_name(event);
                if (k.empty()) k = "";

                if (k == "ctrl+x") {
                    /* press Ctrl+X again to close at any layer */
                    state.set_action_sheet(false, 0);
                    return true;
                }

                if (as.action_sheet_menu == 2) {
                    /* text input layer: characters append, backspace
                       deletes, Enter submits, Esc cancels */
                    if (k == "enter" || k == "\r") {
                        std::string name = as.action_sheet_input;
                        std::string ctx = as.action_sheet_ctx;
                        std::string cur_pl = as.current_playlist_id;
                        state.set_action_sheet(false, 0);
                        if (!name.empty()) {
                            std::thread([name, ctx, cur_pl]() {
                                if (ctx == "rename") {
                                    g_ne->playlist_rename(cur_pl.c_str(), name.c_str());
                                } else {
                                    char nid[32] = {0};
                                    if (g_ne->playlist_create(name.c_str(), nid, sizeof(nid)) == 0 && nid[0])
                                        LOG_INFO("created playlist %s", nid);
                                }
                            }).detach();
                        }
                        return true;
                    }
                    if (k == "escape") {
                        state.set_action_sheet(false, 0);
                        return true;
                    }
                    if (k == "backspace") {
                        std::string q = as.action_sheet_input;
                        if (!q.empty()) {
                            int n = (int)q.size() - 1;
                            while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80) n--;
                            q.resize((size_t)n);
                            state.set_action_sheet_input(q);
                        }
                        return true;
                    }
                    if (k.size() >= 1) {
                        bool control = (unsigned char)k[0] < 32 ||
                                       (unsigned char)k[0] == 127;
                        if (!control)
                            state.set_action_sheet_input(as.action_sheet_input + k);
                        return true;
                    }
                    return true;
                }

                if (as.action_sheet_menu == 3) {
                    /* confirm layer */
                    if (k == "enter" || k == "\r") {
                        std::string id = as.action_sheet_ctx;
                        state.set_action_sheet(false, 0);
                        std::thread([id]() {
                            LOG_INFO("ACTION SHEET: delete playlist %s -> %d",
                                     id.c_str(), g_ne->playlist_delete(id.c_str()));
                        }).detach();
                        return true;
                    }
                    if (k == "escape") {
                        state.set_action_sheet(true, 0);
                        state.set_action_sheet_menu(0);
                        return true;
                    }
                    return true;
                }

                /* menu 0 (main) and menu 1 (playlist picker) share
                   j/k navigation + Enter */
                if (as.action_sheet_menu == 5) {
                    /* song detail: read-only, no navigation */
                    return true;
                }
                if (k == "j" || k == "down") {
                    int n = (as.action_sheet_menu == 1)
                        ? (int)as.action_sheet_pls.size()
                        : (as.action_sheet_menu == 4 || as.action_sheet_menu == 6)
                          ? as.action_sheet_quality_count
                        : as.action_sheet_opt_count;
                    if (n <= 0) n = 1;
                    int sel = as.action_sheet_selected;
                    if (as.action_sheet_menu == 4 || as.action_sheet_menu == 6) {
                        /* skip hidden (no-source) tiers */
                        for (int step = 0; step < n; step++) {
                            sel = (sel + 1) % n;
                            if (as.action_sheet_quality_ok.size() > (size_t)sel &&
                                as.action_sheet_quality_ok[sel] != -2)
                                break;
                        }
                        state.set_action_sheet(true, sel);
                    } else {
                        state.set_action_sheet(true, (sel + 1) % n);
                    }
                    return true;
                }
                if (k == "k" || k == "up") {
                    int n = (as.action_sheet_menu == 1)
                        ? (int)as.action_sheet_pls.size()
                        : (as.action_sheet_menu == 4 || as.action_sheet_menu == 6)
                          ? as.action_sheet_quality_count
                        : as.action_sheet_opt_count;
                    if (n <= 0) n = 1;
                    int sel = as.action_sheet_selected;
                    if (as.action_sheet_menu == 4 || as.action_sheet_menu == 6) {
                        for (int step = 0; step < n; step++) {
                            sel = (sel + n - 1) % n;
                            if (as.action_sheet_quality_ok.size() > (size_t)sel &&
                                as.action_sheet_quality_ok[sel] != -2)
                                break;
                        }
                        state.set_action_sheet(true, sel);
                    } else {
                        state.set_action_sheet(true, (sel + n - 1) % n);
                    }
                    return true;
                }
                if (k == "escape") {
                    if (as.action_sheet_menu == 1 || as.action_sheet_menu == 4 ||
                        as.action_sheet_menu == 5 || as.action_sheet_menu == 6) {
                        state.set_action_sheet_menu(0);
                        state.set_action_sheet(true, 0);
                    } else {
                        state.set_action_sheet(false, 0);
                    }
                    return true;
                }
                if (k == "g" || k == "G") {
                    /* play-quality picker: set the selected level as the
                       global default (all songs) */
                    if (as.action_sheet_menu == 6) {
                        static const char *const kLevels[] = {
                            "jymaster", "sky", "jyeffect", "hires", "lossless",
                            "exhigh", "higher", "standard"
                        };
                        int qi = as.action_sheet_selected;
                        if (qi < 0 || qi >= (int)(sizeof(kLevels)/sizeof(kLevels[0])))
                            return true;
                        std::string lvl = kLevels[qi];
                        std::string notice;
                        if (nq_global_set(lvl.c_str()) == 0) {
                            notice = "\u5DF2\u8BBE\u5168\u5C40\u9ED8\u8BA4\u97F3\u8D28: " + lvl;  /* 已设全局默认音质 */
                            /* the global default changed → the playing track
                               only restarts if it has NO per-song override */
                            quality_changed_refresh(nullptr);
                        } else {
                            notice = "\u8BBE\u7F6E\u5168\u5C40\u97F3\u8D28\u5931\u8D25";  /* 设置全局音质失败 */
                        }
                        state.set_action_sheet(false, 0);
                        StateStore::instance().set_app_notice(notice, 1);
                        return true;
                    }
                }
                if (k == "enter" || k == "\r") {
                    if (as.action_sheet_menu == 6) {
                        /* play quality picker: set the per-song override to
                           the selected level, then close */
                        static const char *const kLevels[] = {
                            "jymaster", "sky", "jyeffect", "hires", "lossless",
                            "exhigh", "higher", "standard"
                        };
                        int qi = as.action_sheet_selected;
                        if (qi < 0 || qi >= (int)(sizeof(kLevels)/sizeof(kLevels[0])))
                            return true;
                        /* a hidden (no-source) tier can't be chosen */
                        if ((int)as.action_sheet_quality_ok.size() > qi &&
                            (as.action_sheet_quality_ok[qi] == -2 ||
                             as.action_sheet_quality_ok[qi] == -1)) {
                            StateStore::instance().set_app_notice(
                                "\u8BE5\u97F3\u8D28\u65E0\u6E90\u53EF\u7528", 3);  /* 该音质无源可用 */
                            return true;
                        }
                        /* the picker's target song is carried in
                           action_sheet_quality_id (set when the picker
                           opened), NOT as.playlist[selected_index] — the
                           selection index here indexes quality levels, not
                           the surrounding song list. */
                        std::string qid = as.action_sheet_quality_id;
                        if (qid.empty()) return true;
                        std::string lvl = kLevels[qi];
                        std::string notice;
                        if (lvl == nq_global_level()) {
                            nq_override_del(qid.c_str());
                            notice = "\u5DF2\u56DE\u590D\u5168\u5C40\u97F3\u8D28: " + lvl;  /* 已恢复全局音质 */
                        } else {
                            nq_override_set(qid.c_str(), lvl.c_str());
                            notice = "\u5DF2\u8BBE\u6B64\u66F2\u97F3\u8D28: " + lvl;  /* 已设此曲音质 */
                        }
                        /* refresh the mirror + reopen the playing track if
                           this song is the one currently playing */
                        quality_changed_refresh(qid.c_str());
                        state.set_action_sheet(false, 0);
                        StateStore::instance().set_app_notice(notice, 1);
                        return true;
                    }
                    if (as.action_sheet_menu == 4) {
                        /* download quality picker: Enter starts the download
                           for the selected level. Denied levels (probed as
                           unusable via the download api) are refused with a
                           notice. */
                        static const char *const kLevels[] = {
                            "jymaster", "sky", "jyeffect", "hires", "lossless",
                            "exhigh", "higher", "standard"
                        };
                        int qi = as.action_sheet_selected;
                        if (qi < 0 || qi >= (int)(sizeof(kLevels)/sizeof(kLevels[0])))
                            return true;
                        /* a hidden (no-source) tier can't be entered: move to
                           the first visible one */
                        if ((int)as.action_sheet_quality_ok.size() > qi &&
                            as.action_sheet_quality_ok[qi] == -2) {
                            for (int i = 0; i < (int)(sizeof(kLevels)/sizeof(kLevels[0])); i++) {
                                if ((int)as.action_sheet_quality_ok.size() <= i ||
                                    as.action_sheet_quality_ok[i] != -2) {
                                    qi = i;
                                    break;
                                }
                            }
                            state.set_action_sheet(true, qi);
                        }
                        /* the picker's target song is carried in
                           action_sheet_quality_id, not as.playlist[] (the
                           selection here is a quality level, not a song). */
                        std::string qid = as.action_sheet_quality_id;
                        if (qid.empty()) return true;
                        /* pure result-driven: the probe already asked the
                           download endpoint per tier; 1 = served (download),
                           2 = denied (warning), -1 = still probing (allow —
                           the ladder falls back anyway), -2 = no source. */
                        if ((int)as.action_sheet_quality_ok.size() > qi) {
                            int qs = as.action_sheet_quality_ok[qi];
                            if (qs == 2) {
                                StateStore::instance().set_app_notice(
                                    "\u8BE5\u97F3\u8D28\u4E0D\u53EF\u4E0B\u8F7D", 3);  /* 该音质不可下载 */
                                return true;
                            }
                            if (qs == 0) {  /* legacy gated marker — defensive */
                                StateStore::instance().set_app_notice(
                                    "\u8BE5\u97F3\u8D28\u4E0D\u53EF\u4E0B\u8F7D", 3);  /* 该音质不可下载 */
                                return true;
                            }
                        }
                        std::string lvl = kLevels[qi];
                        std::string title = !as.action_sheet_quality_title.empty()
                            ? as.action_sheet_quality_title : qid;
                        std::string artist = as.action_sheet_quality_artist;
                        /* don't enqueue a duplicate of an active task for the
                           same song+level (the sheet stays open after Enter,
                           so a re-Enter on a downloading tier is possible) */
                        for (const auto &d : as.downloads) {
                            if (d.id == qid && d.level == lvl) {
                                StateStore::instance().set_app_notice(
                                    "\u5DF2\u5728\u4E0B\u8F7D\u961F\u5217");  /* 已在下载队列 */
                                return true;
                            }
                        }
                        /* Keep the picker open after starting the download so
                           the user can queue more quality levels (and watch
                           the per-level spinner in the sheet). */
                        DownloadQueue::instance().enqueue(qid, title, artist, lvl);
                        return true;
                    }
                    const auto &item = as.playlist[as.selected_index];
                    std::string id = item.id ? item.id : "";
                    if (as.action_sheet_menu == 1) {
                        /* playlist picker: add song to selected playlist */
                        if (as.action_sheet_selected >= 0 &&
                            as.action_sheet_selected < (int)as.action_sheet_pls.size()) {
                            std::string plid = as.action_sheet_pls[as.action_sheet_selected].id ? as.action_sheet_pls[as.action_sheet_selected].id : "";
                            state.set_action_sheet(false, 0);
                            if (!plid.empty() && !id.empty()) {
                                std::thread([plid, id]() {
                                    LOG_INFO("ACTION SHEET: add %s to %s -> %d",
                                             id.c_str(), plid.c_str(),
                                             g_ne->track_add(plid.c_str(), id.c_str()));
                                }).detach();
                            }
                        }
                        return true;
                    }
                    /* menu 0 — selection index matches render order
                       (playlist: 0 subscribe, 1 rename, 2 delete;
                        song:     0 like, 1 add-to-playlist, 2 download,
                                  3 detail, 4 remove-own) */
                    bool is_pl = item.is_playlist;
                    /* active<0 = state query still in flight: treat as
                       inactive (idempotent on the API side) */
                    bool active = as.action_sheet_active == 1;
                    bool in_own = is_pl ? (item.mine == 1)
                                        : as.detail_playlist_mine;
                    int idx = as.action_sheet_selected;
                    if (id.empty()) return true;
                    if (is_pl) {
                        if (idx == 0) {
                            /* subscribe/unsubscribe */
                            std::thread([id, active]() {
                                int rv = g_ne->subscribe_playlist(id.c_str(), !active);
                                LOG_INFO("ACTION SHEET: subscribe %s -> %d", id.c_str(), rv);
                                if (rv == 0)
                                    StateStore::instance().set_action_sheet_active(active ? 0 : 1);
                            }).detach();
                        } else if (idx == 1 && in_own) {
                            /* rename */
                            state.set_action_sheet_ctx("rename");
                            state.set_action_sheet_input("");
                            state.set_action_sheet_menu(2);
                        } else if (idx == 2 && in_own) {
                            /* delete (confirm) */
                            state.set_action_sheet_ctx(id);
                            state.set_action_sheet_menu(3);
                        }
                    } else {
                        if (idx == 0) {
                            /* like/unlike */
                            std::thread([id, active]() {
                                int rv = g_ne->like_song(id.c_str(), !active);
                                LOG_INFO("ACTION SHEET: like %s -> %d", id.c_str(), rv);
                                if (rv == 0)
                                    StateStore::instance().set_action_sheet_active(active ? 0 : 1);
                            }).detach();
                        } else if (idx == 1) {
                            /* add to playlist: load my playlists */
                            state.set_action_sheet_menu(1);
                            state.set_action_sheet(true, 0);
                            std::thread([]() {
                                SongInfo *pls = NULL; int pc = 0;
                                if (g_ne->playlists(false, &pls, &pc) == 0 && pc > 0) {
                                    std::vector<SongInfo> vec;
                                    for (int i = 0; i < pc; i++)
                                        vec.push_back(pls[i]);
                                    free(pls);
                                    StateStore::instance().set_action_sheet_pls(vec);
                                } else {
                                    StateStore::instance().set_action_sheet_pls({});
                                }
                            }).detach();
                        } else if (idx == 2) {
                            /* download → quality picker (menu 4). Levels are
                               listed high→low (jymaster…standard). A tier
                               with no source in the track's per-tier source
                               table (song-music-quality) is hidden (-2);
                               tiers that exist are shown with their bitrate. */
                            std::vector<int> ok(NQ_LEVELS, -1);
                            StateStore::instance().set_action_sheet_quality(id, ok);
                            StateStore::instance().set_action_sheet_quality_meta(
                                item.title ? item.title : "", item.artist ? item.artist : "");
                            state.set_action_sheet_menu(4);
                            state.set_action_sheet(true, 0);
                            /* kick off the authoritative source probe. The
                               picker shows a spinner until it lands (then the
                               level list appears with bitrates). Eligibility
                               mirrors the client: free tiers always available;
                               sq/je need black-vinyl VIP (VIP colour); sk/jm
                               need SVIP (SVIP colour); VIP-paid tracks are
                               only downloadable when owned. */
                            StateStore::instance().set_action_sheet_quality_probing(true);
                            std::thread([id]() {
                                /* Pure result-driven entitlement: the download
                                   endpoint's per-tier answer is the authority
                                   (code 200 + real url = downloadable, -120 /
                                   no url = denied). We do NOT infer from VIP
                                   level / purchased list / fee — the server
                                   decides, "能下就是能下". The source table
                                   (song-music-quality) is used only to skip
                                   tiers that have no file at all (-2 hidden);
                                   every remaining tier is then probed via the
                                   download endpoint for the real verdict. */
                                unsigned mask = 0;
                                int br[NQ_LEVELS] = {0};
                                std::vector<int> res(NQ_LEVELS, -1);
                                static const unsigned bits[NQ_LEVELS] = {
                                    NQ_JYMASTER, NQ_SKY, NQ_JYEFFECT, NQ_HIRES,
                                    NQ_LOSSLESS, NQ_EXHIGH, NQ_HIGHER, NQ_STANDARD
                                };
                                static const char *const kLv[NQ_LEVELS] = {
                                    "jymaster", "sky", "jyeffect", "hires",
                                    "lossless", "exhigh", "higher", "standard"
                                };
                                if (g_ne->song_music_quality(id.c_str(), &mask, br) == 0) {
                                    std::vector<int> brv(br, br + NQ_LEVELS);
                                    StateStore::instance().set_action_sheet_quality_br(id, brv);
                                    for (int i = 0; i < NQ_LEVELS; i++) {
                                        if (!(mask & bits[i])) { res[i] = -2; continue; }
                                        char url[1024];
                                        if (g_ne->download_url(id.c_str(), kLv[i],
                                                               url, sizeof url) == 0)
                                            res[i] = 1;   /* server serves it → downloadable */
                                        else
                                            res[i] = 2;   /* denied → warning (not downloadable) */
                                    }
                                } else {
                                    res = { -2, -2, -2, -2, -2, -2, -2, -2 };
                                }
                                StateStore::instance().set_action_sheet_quality(id, res);
                                /* a tier that turned out hidden (no source)
                                   must not keep the selection: park on the
                                   first visible one so the `>` marker is
                                   immediately on a real entry. */
                                const auto &st = StateStore::instance().state();
                                int sel = st.action_sheet_selected;
                                if ((size_t)sel < res.size() && res[sel] == -2) {
                                    int target = 0;
                                    for (; target < NQ_LEVELS; target++)
                                        if ((size_t)target >= res.size() ||
                                            res[target] != -2) break;
                                    StateStore::instance().set_action_sheet(true, target);
                                }
                                StateStore::instance().set_action_sheet_quality_probing(false);
                            }).detach();
                        } else if (idx == 3) {
                            /* play quality → picker (menu 6): per-song override
                               (default = follow global). Only tiers the track
                               actually has a source for are shown; the source
                               table is served from the quality cache (probe +
                               cache on miss). Current selection is marked,
                               Enter sets the override. */
                            char cur[32] = {0};
                            std::string cur_str = nq_global_level();
                            if (nq_override_get(id.c_str(), cur, sizeof cur) == 0)
                                cur_str = cur;
                            StateStore::instance().set_action_sheet_menu(6);
                            state.set_action_sheet(true, 0);
                            StateStore::instance().set_action_sheet_quality_probing(true);
                            StateStore::instance().set_action_sheet_quality_meta(
                                item.title ? item.title : "", item.artist ? item.artist : "");
                            /* initial state: all tiers probing (-1) */
                            std::vector<int> ok(NQ_LEVELS, -1);
                            StateStore::instance().set_action_sheet_quality(id, ok);
                            std::thread([id, cur_str]() {
                                /* Pure result-driven (mirrors the download
                                   picker): the play endpoint (song-url) is the
                                   authority for "can this tier stream" — code
                                   200 + a real url = playable, anything else
                                   (denied / no url) = not. No inference from
                                   VIP level / purchased list / fee. The source
                                   table is used only to skip tiers that have
                                   no file at all (-2 hidden). */
                                unsigned mask = 0;
                                int br[NQ_LEVELS] = {0};
                                if (nq_cache_get(id.c_str(), &mask, br) != 0)
                                    g_ne->song_music_quality(id.c_str(), &mask, br);
                                std::vector<int> res(NQ_LEVELS, -2);
                                static const unsigned bits[NQ_LEVELS] = {
                                    NQ_JYMASTER, NQ_SKY, NQ_JYEFFECT, NQ_HIRES,
                                    NQ_LOSSLESS, NQ_EXHIGH, NQ_HIGHER, NQ_STANDARD
                                };
                                static const char *const kNames[] = {
                                    "jymaster", "sky", "jyeffect", "hires",
                                    "lossless", "exhigh", "higher", "standard"
                                };
                                for (int i = 0; i < NQ_LEVELS; i++) {
                                    if (!(mask & bits[i])) continue;  /* no source → hidden */
                                    char url[1024];
                                    int playable =
                                        (g_ne->song_url(id.c_str(), kNames[i],
                                                        url, sizeof url) == 0);
                                    if (playable)
                                        res[i] = (cur_str == kNames[i]) ? 1 : 0;
                                    else
                                        res[i] = 2;  /* denied → warning */
                                }
                                if (mask) nq_cache_put(id.c_str(), mask, br);
                                std::vector<int> brv(br, br + NQ_LEVELS);
                                StateStore::instance().set_action_sheet_quality_br(id, brv);
                                StateStore::instance().set_action_sheet_quality(id, res);
                                StateStore::instance().set_action_sheet_quality_probing(false);
                                /* park selection on the first *entitled*
                                   tier (1 = current, 0 = playable), so the
                                   default highlight isn't a no-entitlement
                                   tier that the server would downgrade. Fall
                                   back to any visible tier if none is. */
                                int first_vis = -1;
                                for (int i = 0; i < NQ_LEVELS; i++) {
                                    if (res[i] == -2) continue;
                                    if (first_vis < 0) first_vis = i;
                                    if (res[i] == 1 || res[i] == 0) {
                                        StateStore::instance().set_action_sheet(true, i);
                                        break;
                                    }
                                }
                                if (first_vis >= 0)
                                    StateStore::instance().set_action_sheet(
                                        true, first_vis);
                            }).detach();
                        } else if (idx == 4) {
                            /* song detail (merged from the old d-key popup) */
                            StateStore::instance().set_song_detail({"  \u52A0\u8F7D\u4E2D..."});  /* 加载中... */
                            state.set_action_sheet_menu(5);
                            state.set_action_sheet(true, 0);
                            std::thread([id]() {
                                SongDetail d;
                                if (g_ne->song_detail(id.c_str(), &d) != 0) {
                                    StateStore::instance().set_song_detail(
                                        {"  \u83B7\u53D6\u5931\u8D25" });  /* 获取失败 */
                                    return;
                                }
                                std::vector<std::string> lines;
                                char buf[128];
                                std::string t = d.title ? d.title : "?";
                                std::string a = d.artist ? d.artist : "?";
                                std::string al = d.album ? d.album : "?";
                                lines.push_back("  \u6B4C\u66F2:  " + t);     /* 歌曲 */
                                lines.push_back("  \u6B4C\u624B:  " + a);     /* 歌手 */
                                lines.push_back("  \u4E13\u8F91:  " + al);    /* 专辑 */
                                int m = d.duration_sec / 60, s2 = d.duration_sec % 60;
                                snprintf(buf, sizeof(buf), "%d:%02d", m, s2);
                                lines.push_back("  \u65F6\u957F:  " + std::string(buf));  /* 时长 */
                                const char *fees[] = {"\u514D\u8D39", "\u4F1A\u5458", "\u4ED8\u8D39", "", "", "", "", "", "\u4F1A\u5458"};
                                const char *fee = (d.fee >= 0 && d.fee < 9) ? fees[d.fee] : "";
                                lines.push_back("  \u8D39\u7528:  " + std::string(fee ? fee : "?"));  /* 费用 */
                                if (d.publish) lines.push_back("  \u53D1\u884C:  " + std::string(d.publish));  /* 发行 */
                                if (d.pop >= 0) {
                                    snprintf(buf, sizeof(buf), "%d/100", d.pop);
                                    lines.push_back("  \u70ED\u5EA6:  " + std::string(buf));  /* 热度 */
                                }
                                g_ne->song_detail_free(&d);
                                StateStore::instance().set_song_detail(lines);
                            }).detach();
                        } else if (idx == 5 && in_own) {
                            /* remove from current playlist */
                            std::string cid = as.current_playlist_id;
                            state.set_action_sheet(false, 0);
                            if (!cid.empty() && !id.empty()) {
                                std::thread([cid, id]() {
                                    LOG_INFO("ACTION SHEET: remove %s from %s -> %d",
                                             id.c_str(), cid.c_str(),
                                             g_ne->track_remove(cid.c_str(), id.c_str()));
                                }).detach();
                            }
                        }
                    }
                    return true;
                }
                return true;  /* swallow all keys while open */
            }
        }

        /* ── Mouse: click to select row / wheel to scroll ── */
        if (event.is_mouse()) {
            const AppState &st = state.state();
            if (st.login_state != 0 || st.show_help || st.lyric_mode)
                return true;  /* overlays: ignore the mouse */
            auto &m = event.mouse();
            /* Wheel: move the selection in the panel under the cursor */
            if (m.button == ftxui::Mouse::WheelUp ||
                m.button == ftxui::Mouse::WheelDown) {
                int dir = (m.button == ftxui::Mouse::WheelUp) ? -1 : 1;
                if (m.x >= 20) {
                    /* right song panel */
                    StateStore::instance().set_active_panel(1);
                    /* search box focused: the filtered view maps rows
                       1:1 to matches, so raw selected_index moves would
                       jump erratically — drop back to the full list */
                    if (st.top_search_active) {
                        close_top_search();
                        if (dir < 0) return true;  /* up: just exit */
                    }
                    int n = (int)st.playlist.size();
                    if (n > 0) {
                        int idx = st.selected_index + dir;
                        if (idx < 0) idx = 0;
                        if (idx >= n) idx = n - 1;
                        StateStore::instance().set_selected_index(idx);
                    }
                } else {
                    /* left menu/groups panel — hover-only, like the
                       keyboard: the right panel fills only on Enter */
                    StateStore::instance().set_active_panel(0);
                    if (st.music_mode == MusicMode::Local) {
                        int n = (int)st.groups.size();
                        if (n > 0) {
                            int idx = st.group_index + dir;
                            if (idx < -1) idx = -1;
                            if (idx >= n) idx = n - 1;
                            StateStore::instance().set_group_hover(idx);
                        }
                    } else {
                        int n = (int)st.netease_menu.size();
                        if (n > 0) {
                            int idx = st.netease_selected + dir;
                            if (idx < -1) idx = -1;
                            if (idx >= n) idx = n - 1;
                            StateStore::instance().set_netease_selected(idx);
                        }
                    }
                }
                return true;
            }
            if (m.button != ftxui::Mouse::Left ||
                m.motion != ftxui::Mouse::Pressed)
                return true;
            if (m.y < 1 || m.y > st.screen_height - 3)
                return true;  /* top/status bars */
            if (m.x >= 20) {
                /* right panel: row = y-2 (top bar 1 + border 1) + offset.
                   Inside the DOWNLOADS group (only when actually entered)
                   the leading download-task rows are informational —
                   subtract them so clicks map to the file rows. */
                int row = (m.y - 2) + st.song_list_offset;
                bool in_dl = (st.active_panel == 1 &&
                              st.music_mode == MusicMode::Local &&
                              st.group_index >= 0 &&
                              st.group_index < (int)st.groups.size() &&
                              st.groups[st.group_index].is_downloads);
                if (in_dl) row -= (int)st.downloads.size();
                StateStore::instance().set_active_panel(1);
                if (st.top_search_active)
                    StateStore::instance().set_top_search_active(false);
                if (row >= 0 && row < (int)st.playlist.size()) {
                    if (row == st.selected_index)
                        component->OnEvent(ftxui::Event::Return);  /* re-click = activate */
                    else
                        StateStore::instance().set_selected_index(row);
                }
            } else {
                /* left menu: rows are [nav][sep]menu... in netease mode,
                   [nav]groups... in local mode (hidden while searching) */
                int row = m.y - 2;
                bool searching = st.top_search_active;
                StateStore::instance().set_active_panel(0);
                if (st.music_mode == MusicMode::Local) {
                    if (searching) return true;  /* filtered view: ignore */
                    if (row == 0) {
                        if (st.group_index == -1)
                            component->OnEvent(ftxui::Event::Return);
                        else
                            StateStore::instance().set_group_hover(-1);
                    } else if (row - 1 < (int)st.groups.size()) {
                        /* hover-only like the keyboard: a click moves the
                           highlight; a second click on the same row (or
                           Enter) loads the group's content. */
                        if (row - 1 == st.group_index)
                            component->OnEvent(ftxui::Event::Return);
                        else
                            StateStore::instance().set_group_hover(row - 1);
                    }
                } else {
                    if (searching) return true;  /* filtered view: ignore */
                    /* layout (netease, query empty): menu[0..n-1],
                       local entry as the last row at the bottom */
                    int n = (int)st.netease_menu.size();
                    if (row < n) {
                        if (row == st.netease_selected)
                            component->OnEvent(ftxui::Event::Return);
                        else
                            StateStore::instance().set_netease_selected(row);
                    } else if (row == n) {
                        /* bottom local entry */
                        if (st.netease_selected == -1)
                            component->OnEvent(ftxui::Event::Return);
                        else
                            StateStore::instance().set_netease_selected(-1);
                    }
                }
            }
            return true;
        }

        /* ── Non-seek key → discard pending seek ── */
        if (state.state().seek_accum != 0 && event != ftxui::Event::ArrowLeft && event != ftxui::Event::ArrowRight) {
            state.set_seek_accum(0);
            state.set_seek_target(-1);
            state.set_seek_indicator(0);
            state.set_seek_target_progress(0.0f);
        }

        /* ── Input modes accept characters (IME text) directly ──
           Character events (incl. multi-byte UTF-8) keep their raw text
           so the search boxes below can append them. Non-input modes
           map keys via event_to_key_name() (IME text is filtered out). */
        const AppState &cur = state.state();
        bool input_mode = (cur.top_search_active && !cur.lyric_mode) ||
                          (cur.search_active && cur.music_mode != MusicMode::Netease);
        std::string ev_key;
        if (input_mode && event.is_character()) {
            const std::string &ch = event.character();
            /* control characters (Ctrl+/ etc.) are keybinding keys, not
               query text — route them through the key-name mapping */
            bool control = ch.size() == 1 &&
                           ((unsigned char)ch[0] < 32 || (unsigned char)ch[0] == 127);
            if (control) {
                ev_key = event_to_key_name(event);
            } else {
                ev_key = ch;
                if (ev_key == " ") ev_key = "space";
            }
        } else {
            ev_key = event_to_key_name(event);
        }
        if (ev_key.empty()) return false;

        /* ── Help screen: Esc to close ── */
        if (cur.show_help && ev_key == "escape") {
            StateStore::instance().set_show_help(false);
            return true;
        }

        /* ── Lyrics mode: Esc to close ── */
        if (cur.lyric_mode && ev_key == "escape") {
            StateStore::instance().set_lyric_mode(false);
            return true;
        }

        /* ── Login overlay: Esc to close ── */
        if (cur.login_state != 0 && (ev_key == "escape")) {
            StateStore::instance().set_login_state(0, "", "");
            g_login_unikey.clear();
            return true;
        }

        /* ── Top search row input (both modes) ───────── */
        if (cur.top_search_active && !cur.lyric_mode) {
            if (ev_key == "escape") {
                /* Esc leaves search and restores the pre-search view */
                close_top_search();
                return true;
            }
            if (ev_key == "ctrl+/") {
                /* same as Esc: exit the box back to the list */
                close_top_search();
                return true;
            }
            if (ev_key == "up" || ev_key == "down") {
                /* Cursor model: the search box and the list are one
                   continuous cursor. Down leaves the box and lands on
                   the FIRST visible item (first match when filtering) —
                   a deterministic target, never a hidden selection. */
                StateStore::instance().set_top_search_active(false);
                const auto &st2 = StateStore::instance().state();
                if (!cur.top_search_api && !cur.top_right_query.empty() &&
                    !st2.playlist.empty()) {
                    int m = next_match(st2.playlist, -1, 1,
                                       cur.top_right_query);
                    StateStore::instance().set_selected_index(m >= 0 ? m : 0);
                } else {
                    StateStore::instance().set_selected_index(0);
                }
                return true;
            }
            if (ev_key == "enter" || ev_key == "\r") {
                /* right box:
                   - normal (non-netease) mode: Enter does nothing —
                     the box stays selected & editable
                   - netease mode: Enter submits the API search and
                     leaves the box so the results are interactive */
                if (cur.top_search_api &&
                    !cur.top_right_query.empty()) {
                    if (!netease_search_apply_cache(cur.top_right_query)) {
                        do_netease_search(cur.top_right_query.c_str(),
                                          !state.state().top_search_pushed);
                        state.set_top_search_pushed(true);
                    }
                    StateStore::instance().set_top_search_active(false);
                    StateStore::instance().set_active_panel(1);
                }
                return true;
            }
            if (ev_key == "backspace") {
                /* remove last char (UTF-8 safe) */
                std::string q = cur.top_right_query;
                if (!q.empty()) {
                    int n = (int)q.size() - 1;
                    while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80) n--;
                    q.resize((size_t)n);
                }
                StateStore::instance().set_top_right_query(q);
                if (!cur.top_search_api) {
                    restore_search_view(q);
                    const auto &st2 = StateStore::instance().state();
                    if (!q.empty() && !st2.playlist.empty()) {
                        int m = next_match(st2.playlist, -1, 1, q);
                        if (m >= 0)
                            StateStore::instance().set_selected_index(m);
                    }
                }
                return true;
            }
            /* character (ASCII or UTF-8 from IME): append to query.
               (input_mode guarantees ev_key is a character here) */
            {
                /* navigation keys must not become query text — up/down
                   already handled above, left/right have no edit-cursor
                   here so they are simply dropped */
                if (ev_key == "left" || ev_key == "right" ||
                    ev_key == "up" || ev_key == "down")
                    return true;
                std::string ch = ev_key;
                if (ev_key == "space") ch = " ";
                std::string q = cur.top_right_query + ch;
                StateStore::instance().set_top_right_query(q);
                /* API-search box keeps the list untouched while typing —
                   Enter submits a fresh API search (no "search within
                   results" fallback); filter boxes react live */
                if (!cur.top_search_api) {
                    restore_search_view(q);
                    /* snap the selection to the first matching row so
                       Enter/space never acts on an invisible item */
                    const auto &st2 = StateStore::instance().state();
                    if (!q.empty() && !st2.playlist.empty()) {
                        int m = next_match(st2.playlist, -1, 1, q);
                        if (m >= 0)
                            StateStore::instance().set_selected_index(m);
                    }
                }
            }
            return true; /* consume all keys while top search active */
        }

        /* ── Search input mode: capture keys as query text ── */
        if (cur.search_active && cur.music_mode != MusicMode::Netease) {
            /* Navigate results */
            /* scope=0 (filter): navigate within current playlist */
            if (cur.search_scope == 0) {
                if (ev_key == "up") {
                    /* find previous matching item */
                    std::string q = cur.search_query;
                    if (!q.empty()) {
                        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
                        int idx = cur.selected_index;
                        for (int n = 0; n < (int)cur.playlist.size(); n++) {
                            idx = (idx <= 0) ? (int)cur.playlist.size() - 1 : idx - 1;
                            const auto &s = cur.playlist[idx];
                            std::string h;
                            if (s.title) h += s.title;
                            if (s.artist) h += std::string(" ") + s.artist;
                            std::transform(h.begin(), h.end(), h.begin(), ::tolower);
                            if (h.find(q) != std::string::npos) {
                                StateStore::instance().set_selected_index(idx);
                                break;
                            }
                        }
                    } else {
                        int idx = cur.selected_index - 1;
                        if (idx < 0) idx = (int)cur.playlist.size() - 1;
                        StateStore::instance().set_selected_index(idx);
                    }
                    return true;
                }
                if (ev_key == "down") {
                    std::string q = cur.search_query;
                    if (!q.empty()) {
                        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
                        int idx = cur.selected_index;
                        for (int n = 0; n < (int)cur.playlist.size(); n++) {
                            idx = (idx + 1 >= (int)cur.playlist.size()) ? 0 : idx + 1;
                            const auto &s = cur.playlist[idx];
                            std::string h;
                            if (s.title) h += s.title;
                            if (s.artist) h += std::string(" ") + s.artist;
                            std::transform(h.begin(), h.end(), h.begin(), ::tolower);
                            if (h.find(q) != std::string::npos) {
                                StateStore::instance().set_selected_index(idx);
                                break;
                            }
                        }
                    } else {
                        int idx = cur.selected_index + 1;
                        if (idx >= (int)cur.playlist.size()) idx = 0;
                        StateStore::instance().set_selected_index(idx);
                    }
                    return true;
                }
                if (ev_key == "enter" || ev_key == "\r") {
                    /* exit filter and play the selected song */
                    StateStore::instance().set_search_active(false);
                    StateStore::instance().set_search_query("");
                    StateStore::instance().set_search_scope(0);
                    if (cur.active_panel == 1 && !cur.playlist.empty()) {
                        int idx = cur.selected_index;
                        if (idx >= 0 && idx < (int)cur.playlist.size())
                            play_from_playlist(idx);
                    }
                    return true;
                }
            }
            /* scope=1 (global search): navigate results */
            if (ev_key == "up" && !cur.search_results.empty()) {
                int idx = cur.search_selected - 1;
                if (idx < 0) idx = (int)cur.search_results.size() - 1;
                StateStore::instance().set_search_selected(idx);
                return true;
            }
            if (ev_key == "down" && !cur.search_results.empty()) {
                int idx = cur.search_selected + 1;
                if (idx >= (int)cur.search_results.size()) idx = 0;
                StateStore::instance().set_search_selected(idx);
                return true;
            }
            /* Enter: submit search in netease mode, navigate in local mode */
            if (ev_key == "enter" || ev_key == "\r") {
                if (cur.music_mode == MusicMode::Netease) {
                    if (!cur.search_query.empty()) {
                        /* submit search to API (legacy path — netease mode
                           now uses the top search row instead) */
                        do_netease_search(cur.search_query.c_str(), true);
                    }
                    return true;
                }
                /* scope=1 local: navigate to selected result */
                if (cur.search_scope == 1 && !cur.search_results.empty()) {
                    const auto &sel = cur.search_results[cur.search_selected];
                    if (sel.source && strcmp(sel.source, "local") == 0 &&
                        sel.id && sel.id[0]) {
                        int target_group = -1;
                        int target_song = -1;
                        for (int gi = 0; gi < (int)cur.groups.size() && target_group < 0; gi++) {
                            for (int si = 0; si < (int)cur.groups[gi].songs.size(); si++) {
                                if (strcmp(cur.groups[gi].songs[si].id, sel.id) == 0) {
                                    target_group = gi;
                                    target_song = si;
                                    break;
                                }
                            }
                        }
                        if (target_group >= 0) {
                            std::vector<const char*> paths;
                            for (auto &s : cur.groups[target_group].songs)
                                paths.push_back(s.id);
                            StateStore::instance().set_group_index(target_group);
                            StateStore::instance().set_selected_index(target_song >= 0 ? target_song : 0);
                            StateStore::instance().set_active_panel(1);
                        }
                    }
                    search_manager_clear();
                    StateStore::instance().set_search_active(false);
                    StateStore::instance().set_search_query("");
                }
                return true;
            }
            if (ev_key == "escape") {
                /* close search */
                search_manager_clear();
                StateStore::instance().set_search_active(false);
                StateStore::instance().set_search_query("");
                return true;
            }
            /* Whether in search mode or viewing search results, backspace
               on empty query or Esc restores previous playlist. Handled
               in the non-search event handler below when cur contains
               the results. */
            if (ev_key == "backspace") {
                /* remove last char (UTF-8 safe) */
                std::string q = cur.search_query;
                if (!q.empty()) {
                    int n = (int)q.size() - 1;
                    while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80) n--;
                    q.resize((size_t)n);
                }
                StateStore::instance().set_search_query(q);
                StateStore::instance().set_search_results({}, 0);
                /* scope=1 local: real-time search; scope=0: just filter */
                if (!q.empty() && cur.music_mode != MusicMode::Netease && cur.search_scope == 1)
                    search_manager_search_source("local", q.c_str(), 0);
                return true;
            }
            /* character (ASCII or UTF-8 from IME): append to query.
               (input_mode guarantees ev_key is a character here) */
            {
                /* navigation keys must not become query text */
                if (ev_key == "left" || ev_key == "right" ||
                    ev_key == "up" || ev_key == "down")
                    return true;
                std::string ch = ev_key;
                if (ev_key == "space") ch = " ";
                std::string q = cur.search_query + ch;
                StateStore::instance().set_search_query(q);
                StateStore::instance().set_search_results({}, 0);
                /* scope=1 local: real-time */
                if (cur.music_mode != MusicMode::Netease && cur.search_scope == 1)
                    search_manager_search_source("local", q.c_str(), 0);
            }
            return true; /* consume all keys while searching */
        }

        /* Esc: navigate back one level (pop the nav stack). The right
           panel list is intentionally NOT restored by nav_pop — its
           content is replaced only by newly loaded content. */
        if (ev_key == "escape" && !cur.search_active) {
            /* Esc on a search-result/filtered list first goes back to
               the search box (query kept) when a query is present; the
               second Esc (box) restores the original list. */
            if (!cur.top_right_query.empty()) {
                StateStore::instance().set_top_search_active(true);
                return true;
            }
            if (!cur.nav_stack.empty()) {
                StateStore::instance().nav_pop();
                StateStore::instance().set_current_playlist_id("");
                StateStore::instance().set_detail_playlist_mine(false);
                return true;
            }
        }

        auto action = g_keybindings.lookup(ev_key);
        if (!action.has_value()) return false;

        switch (action.value()) {

        case Action::Quit:
            screen.ExitLoopClosure()();
            return true;

        case Action::PanelSwitch:
            StateStore::instance().set_active_panel(cur.active_panel ? 0 : 1);
            return true;

        case Action::MoveDown:
            if (cur.active_panel == 0) {
                if (cur.music_mode == MusicMode::Local) {
                    /* local groups: -1 (netease entry) sits at the TOP.
                       Down wraps from the last group back to the entry.
                       Hover only — the right panel fills on Enter. */
                    int n = (int)cur.groups.size();
                    if (n <= 0) return true;
                    int idx = (cur.group_index + 1 >= n) ? -1 : cur.group_index + 1;
                    StateStore::instance().set_group_hover(idx);
                } else {
                    /* netease menu: local entry (-1) sits at the BOTTOM
                       of the list; Down loops through it. */
                    int n = (int)cur.netease_menu.size();
                    if (n <= 0) return true;
                    int sel = cur.netease_selected;          /* -1 = local */
                    int cur_i = (sel < 0) ? n : sel;         /* local -> index n */
                    int next_i = (cur_i + 1 > n) ? 0 : cur_i + 1;
                    StateStore::instance().set_netease_selected(
                        (next_i == n) ? -1 : next_i);
                }
            } else {
                if (cur.playlist.empty()) return true;
                if (!cur.top_search_api && !cur.top_right_query.empty()) {
                    /* filter mode: move between matching rows only */
                    int idx = next_match(cur.playlist, cur.selected_index, 1,
                                         cur.top_right_query);
                    if (idx >= 0)
                        StateStore::instance().set_selected_index(idx);
                } else if (cur.selected_index < (int)cur.playlist.size() - 1) {
                    StateStore::instance().set_selected_index(cur.selected_index + 1);
                }
            }
            return true;

        case Action::MoveUp:
            /* Cursor model: Up from the first list item goes back into
               the search box (editing mode) when a query is active. */
            if (cur.active_panel == 1 && !cur.search_active) {
                bool first_match = false;
                if (cur.top_search_api || cur.top_right_query.empty()) {
                    first_match = (cur.selected_index == 0);
                } else {
                    /* filter mode: at the top matching row when no
                       earlier row matches */
                    first_match = next_match(cur.playlist, cur.selected_index,
                                             -1, cur.top_right_query) < 0;
                }
                if (first_match && !cur.top_right_query.empty()) {
                    StateStore::instance().set_top_search_active(true);
                    return true;
                }
            }
            if (cur.active_panel == 0) {
                if (cur.music_mode == MusicMode::Local) {
                    /* local groups: -1 (netease entry) sits at the TOP.
                       Up from the first group returns to the entry;
                       Up from the entry wraps to the last group.
                       Hover only — the right panel fills on Enter. */
                    int n = (int)cur.groups.size();
                    if (n <= 0) return true;
                    int idx = (cur.group_index <= 0)
                                  ? (cur.group_index == -1 ? n - 1 : -1)
                                  : cur.group_index - 1;
                    StateStore::instance().set_group_hover(idx);
                } else {
                    /* netease menu: local entry (-1) sits at the BOTTOM.
                       Up from the first menu item wraps to local. */
                    int n = (int)cur.netease_menu.size();
                    if (n <= 0) return true;
                    int sel = cur.netease_selected;          /* -1 = local */
                    int cur_i = (sel < 0) ? n : sel;         /* local -> index n */
                    int prev_i = (cur_i - 1 < 0) ? n : cur_i - 1;
                    StateStore::instance().set_netease_selected(
                        (prev_i == n) ? -1 : prev_i);
                }
            } else {
                if (!cur.top_search_api && !cur.top_right_query.empty()) {
                    /* filter mode: move between matching rows only */
                    int idx = next_match(cur.playlist, cur.selected_index, -1,
                                         cur.top_right_query);
                    if (idx >= 0)
                        StateStore::instance().set_selected_index(idx);
                } else if (cur.selected_index > 0) {
                    StateStore::instance().set_selected_index(cur.selected_index - 1);
                }
            }
            return true;

        case Action::PlayPause:
            toggle_playback();
            return true;

        case Action::PlaySelected:
            if (cur.active_panel == 0) {
                /* Left panel: mode switching or menu selection */
                if (cur.music_mode == MusicMode::Local && cur.group_index < 0) {
                    /* Switch to Netease mode */
                    StateStore::instance().set_music_mode(MusicMode::Netease);
                    StateStore::instance().set_active_panel(0);
                    StateStore::instance().set_group_index(-1);
                } else if (cur.music_mode == MusicMode::Netease && cur.netease_selected < 0) {
                    /* Switch back to Local mode. Stay on the group LIST
                       (group_index = -1 = the "网易云音乐" entry) instead
                       of auto-selecting the first group: the right panel
                       only shows songs after the user picks a path. */
                    StateStore::instance().set_music_mode(MusicMode::Local);
                    StateStore::instance().set_active_panel(0);
                    StateStore::instance().set_group_index(-1);
                } else if (cur.music_mode == MusicMode::Local) {
                    /* Enter a local group: push the local-home state onto
                       the nav stack, then show the group's songs on the
                       right (Esc returns to the group list). */
                    if (cur.group_index >= 0 &&
                        cur.group_index < (int)cur.groups.size()) {
                        StateStore::instance().nav_push();
                        StateStore::instance().set_group_index(cur.group_index);
                        StateStore::instance().set_active_panel(1);
                    }
                } else if (cur.music_mode == MusicMode::Netease && cur.netease_selected >= 0) {
                    /* Load netease menu item content into right panel */
                    activate_netease_menu_item(cur.netease_selected);
                }
                return true;
            }
            /* Right panel: play selected song — unless it's a playlist
               item (playlist lists render in the right panel); those
               open the playlist's songs instead. */
            if (cur.active_panel == 1) {
                const auto &song = cur.playlist[cur.selected_index];
                if (song.is_playlist) {
                    StateStore::instance().nav_push_restore_playlist();
                    StateStore::instance().set_current_playlist_id(song.id ? song.id : "");
                    StateStore::instance().set_detail_playlist_mine(song.mine == 1);
                    StateStore::instance().set_loading(true);
                    std::string _pl_id = song.id ? song.id : "";
                    /* purchased digital-album rows carry aux_label "已购专辑"
                       and must open via the album API — the playlist API
                       called with an album id returns unrelated songs. */
                    bool is_purchased_album =
                        song.aux_label &&
                        strcmp(song.aux_label, "已购专辑") == 0;
                    std::thread([_pl_id, is_purchased_album]() {
                        SongInfo *songs = NULL; int sc = 0;
                        int ret = is_purchased_album
                            ? g_ne->album_songs(_pl_id.c_str(), &songs, &sc)
                            : g_ne->playlist_songs(_pl_id.c_str(), &songs, &sc);
                        LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
                        if (ret == 0 && sc > 0) {
                            ld->songs = songs; ld->count = sc;
                        } else {
                            ld->songs = NULL; ld->count = 0;
                            free(songs);
                        }
                        if (event_bus_publish(EV_PLAYLIST_LOADED, ld, sizeof(*ld)) != 0) {
                            if (ld->songs) {
                                for (int i = 0; i < ld->count; i++)
                                    song_info_free(&ld->songs[i]);
                                free(ld->songs);
                            }
                            free(ld);
                        }
                    }).detach();
                } else {
                    play_from_playlist(cur.selected_index);
                }
            }
            return true;

        case Action::NextTrack: {
            auto &store = StateStore::instance();
            int next = store.queue_next();
            if (next < 0) return true;
            play_queue_index(next);
            return true;
        }

        case Action::PrevTrack: {
            auto &store = StateStore::instance();
            int prev = store.queue_prev();
            if (prev < 0) return true;
            play_queue_index(prev);
            return true;
        }

        case Action::VolumeUp: {
            int vol = audio_output_get_volume();
            if (vol >= 0) {
                vol = (vol + 5 > 100) ? 100 : vol + 5;
                event_bus_publish(EV_VOLUME_CHANGED, &vol, sizeof(vol));
            }
            return true;
        }

        case Action::VolumeDown: {
            int vol = audio_output_get_volume();
            if (vol >= 0) {
                vol = (vol - 5 < 0) ? 0 : vol - 5;
                event_bus_publish(EV_VOLUME_CHANGED, &vol, sizeof(vol));
            }
            return true;
        }

        case Action::SeekForward:
            if (cur.playback_state != PlaybackState::Stopped && cur.total_time_sec > 0) {
                int accum = state.state().seek_accum +
                            config_get_int(config_global(), "playback.seek_step_sec", 5);
                state.set_seek_accum(accum);
                state.set_seek_last_ms((int64_t)std::chrono::duration_cast<
                    std::chrono::milliseconds>(std::chrono::steady_clock::now()
                    .time_since_epoch()).count());
                state.set_seek_indicator(accum);
            }
            return true;

        case Action::SeekBackward:
            if (cur.playback_state != PlaybackState::Stopped && cur.total_time_sec > 0) {
                int accum = state.state().seek_accum -
                            config_get_int(config_global(), "playback.seek_step_sec", 5);
                state.set_seek_accum(accum);
                state.set_seek_last_ms((int64_t)std::chrono::duration_cast<
                    std::chrono::milliseconds>(std::chrono::steady_clock::now()
                    .time_since_epoch()).count());
                state.set_seek_indicator(accum);
            }
            return true;

        case Action::Stop:
            StateStore::instance().queue_clear();
            event_bus_publish(EV_PLAYBACK_STOP, NULL, 0);
            return true;

        case Action::ToggleMute: {
            int muted_val = cur.muted ? 0 : 1;
            event_bus_publish(EV_MUTE_CHANGED, &muted_val, sizeof(muted_val));
            return true;
        }

        case Action::OpenSearch: {
            /* search disabled in lyric mode */
            if (cur.lyric_mode) return true;
            /* toggle the top search row (focused panel's box) — both modes.
               Closing just exits the box (focus back to the list); the
               list and query are kept. */
            if (cur.top_search_active) {
                close_top_search();
            } else {
                StateStore::instance().set_top_search_active(true);
                StateStore::instance().set_top_search_api(false);
            }
            return true;
        }

        case Action::ShowHelp:
            StateStore::instance().set_show_help(!cur.show_help);
            return true;

        case Action::ShowActions:
            /* open the action sheet for the selected right-panel item */
            if (!cur.playlist.empty()) {
                StateStore::instance().set_action_sheet(true, 0);
                StateStore::instance().set_action_sheet_active(-1);  /* re-query like/subscribe state */
                StateStore::instance().set_action_sheet_menu(0);
                const auto &item = cur.playlist[cur.selected_index];
                bool is_pl = item.is_playlist;
                bool in_own = is_pl ? (item.mine == 1)
                                    : cur.detail_playlist_mine;
                /* option counts must match action_sheet.cpp's render order:
                   playlist: subscribe + (rename/delete if mine) → 1 / 3
                   song:     like/add/download/play-quality/detail
                            + (remove-if-own) → 5 / 6  (was 4/5 → "歌曲详情"
                            at idx 4 was unreachable via j/k) */
                StateStore::instance().set_action_sheet_opt_count(
                    is_pl ? (in_own ? 3 : 1) : (in_own ? 6 : 5));
                std::string id = item.id ? item.id : "";
                if (!id.empty()) {
                    std::thread([id, is_pl]() {
                        int active = -1;
                        if (is_pl) {
                            SongInfo *pls = NULL; int pc = 0;
                            if (g_ne->playlists(true, &pls, &pc) == 0) {
                                for (int i = 0; i < pc; i++) {
                                    if (pls[i].id && strcmp(pls[i].id, id.c_str()) == 0) {
                                        active = 1; break;
                                    }
                                }
                                for (int i = 0; i < pc; i++) song_info_free(&pls[i]);
                                free(pls);
                                if (active < 0) active = 0;
                            } else {
                                /* no subscribed playlists (or fetch failed):
                                   treat as not subscribed so actions work */
                                active = 0;
                            }
                        } else {
                            bool liked = false;
                            if (g_ne->liked_check(id.c_str(), &liked) == 0)
                                active = liked ? 1 : 0;
                            else
                                active = 0;  /* default: not liked */
                        }
                        StateStore::instance().set_action_sheet_active(active);
                    }).detach();
                }
            }
            return true;

        case Action::CycleLoop: {
            int next = ((int)cur.loop_mode + 1) % 4;
            StateStore::instance().set_loop_mode((LoopMode)next);
            /* persist so the setting survives restarts */
            Config *gcfg = config_global();
            if (gcfg) {
                config_set_int(gcfg, "playback.loop_mode", next);
                config_save(gcfg);
            }
            LOG_INFO("Loop mode: %d", next);
            return true;
        }
        case Action::ToggleLyrics: {
            if (cur.playback_state == PlaybackState::Stopped || !cur.current_song.title)
                return true;  /* nothing playing, ignore */
            bool entering = !cur.lyric_mode;
            StateStore::instance().set_lyric_mode(entering);
            if (entering && cur.current_song.cover_url && cur.current_song.cover_url[0])
                start_cover_download(cur.current_song.cover_url);
            return true;
        }

        default:
            return false;
        }
    });

    /* scoped so the Loop destructor (terminal restore) runs BEFORE the
       shutdown sequence below — a crash in shutdown must not leave the
       terminal in alt-screen limbo */
    {
        ftxui::Loop loop(&screen, component);
        /* seed the resize-detection baseline with the real size so the
           FIRST resize after launch is recognized (and not mistaken for
           Ctrl+/) */
        state.set_last_resize(Terminal::Size().dimx, Terminal::Size().dimy);
        /* Raw-image cover overlay (kitty graphics protocol):
           - upload once per cover (fingerprinted inside term_gfx)
           - re-place IMMEDIATELY whenever the geometry changed (terminal
             resize, new cover, layout shift) so the image never lingers
             at a stale position, then periodically (~0.5s wall clock) to
             heal terminals that drop or relocate placed images on
             fullscreen toggles / window switches
           - clear when leaving lyric mode (edge-triggered) */
        bool gfx_active = false;
        auto gfx_last_place = std::chrono::steady_clock::time_point{};
        int last_dimx = -1, last_dimy = -1;
        int last_cw = -1, last_dw = -1, last_dh = -1;
        uint64_t last_stamp = 0;
        while (!loop.HasQuitted()) {
            loop.RunOnceBlocking();
            if (term_gfx_active()) {
                const AppState &st = state.state();
                bool active = st.lyric_mode && st.cover.pixels &&
                              !st.show_help && st.login_state == 0;
                if (active) {
                    int cw = 0, dw = 0, dh = 0;
                    cover_layout(st, &cw, &dw, &dh);
                    if (dw > 0 && dh > 0) {
                        term_gfx_upload(&st.cover);
                        /* The cover block is centered in the lyric panel,
                           which spans the screen below the 1-row top bar
                           and above the spectrum + 2-row status bar. The
                           spectrum height is ADAPTIVE (2-4 rows, same
                           formula as render_spectrum_bar) — the old fixed
                           "- 2" misplaced the image on tall terminals. */
                        int spec_rows = Terminal::Size().dimy / 12;
                        if (spec_rows < 2) spec_rows = 2;
                        if (spec_rows > 4) spec_rows = 4;
                        int panel_h = Terminal::Size().dimy - 1 - spec_rows - 2;
                        int row0 = 2;
                        if (panel_h > dh)
                            row0 += (panel_h - dh) / 2;
                        /* The cover is centered inside its cw-column slot,
                           mirroring the character renderer / placeholder */
                        int col0 = 1 + cover_left_margin(st) + (cw - dw) / 2;

                        auto now = std::chrono::steady_clock::now();
                        int tdimx = Terminal::Size().dimx;
                        int tdimy = Terminal::Size().dimy;
                        bool geometry_changed =
                            !gfx_active ||
                            tdimx != last_dimx ||
                            tdimy != last_dimy ||
                            cw != last_cw || dw != last_dw || dh != last_dh ||
                            st.cover.stamp != last_stamp;
                        bool due =
                            (now - gfx_last_place) >=
                            std::chrono::milliseconds(500);
                        if (geometry_changed || due) {
                            gfx_last_place = now;
                            last_dimx = tdimx;
                            last_dimy = tdimy;
                            last_cw = cw; last_dw = dw; last_dh = dh;
                            last_stamp = st.cover.stamp;
                            printf("\x1b[%d;%dH", row0, col0);
                            term_gfx_place(dw, dh);
                        }
                    }
                    gfx_active = true;
                } else if (gfx_active) {
                    term_gfx_clear();
                    gfx_active = false;
                    last_dimx = last_dimy = -1;
                    last_cw = last_dw = last_dh = -1;
                    last_stamp = 0;
                }

                /* QR login image: placed over the login layout when the
                   QR screen is up (mutually exclusive with the lyric
                   cover above). Rows are reserved by login_screen as a
                   placeholder starting at row 3; cols = 2x rows keeps
                   the square aspect (half-block cells are 2:1). */
                static bool qr_gfx_placed = false;
                static auto qr_gfx_last = std::chrono::steady_clock::time_point{};
                static int qr_ldx = -1, qr_ldy = -1, qr_lrows = -1;
                static uint64_t qr_lstamp = 0;
                if (g_login_qr_ready && st.login_state == 2) {
                    int rows = st.screen_height - 8;
                    if (rows < 4) rows = 4;
                    if (rows > 12) rows = 12;
                    int cols = rows * 2;
                    int row0 = 3;
                    int col0 = 1 + (Terminal::Size().dimx - cols) / 2;
                    if (col0 < 1) col0 = 1;
                    auto now = std::chrono::steady_clock::now();
                    int tdimx = Terminal::Size().dimx;
                    int tdimy = Terminal::Size().dimy;
                    bool changed = !qr_gfx_placed ||
                                   tdimx != qr_ldx ||
                                   tdimy != qr_ldy ||
                                   rows != qr_lrows ||
                                   g_login_qr.stamp != qr_lstamp;
                    bool due = (now - qr_gfx_last) >=
                               std::chrono::milliseconds(500);
                    if (changed || due) {
                        qr_gfx_last = now;
                        qr_ldx = tdimx;
                        qr_ldy = tdimy;
                        qr_lrows = rows;
                        qr_lstamp = g_login_qr.stamp;
                        term_gfx_upload(&g_login_qr);
                        printf("\x1b[%d;%dH", row0, col0);
                        term_gfx_place(cols, rows);
                    }
                    qr_gfx_placed = true;
                } else if (qr_gfx_placed) {
                    term_gfx_clear();
                    qr_gfx_placed = false;
                    qr_ldx = qr_ldy = -1;
                    qr_lrows = -1;
                    qr_lstamp = 0;
                }
            }
            fflush(stdout);
        }
    }

    LOG_INFO("Shutting down");
    timer_active.store(false);
    refresh_timer.join();
    event_bus_publish(EV_APP_SHUTDOWN, NULL, 0);
#ifndef _WIN32
    mpris_shutdown();
#endif
    playback_coordinator_shutdown();
    music_source_manager_shutdown();
    netease_search_cache_free();
    if (g_thread_pool) threadpool_destroy(g_thread_pool);
    DownloadQueue::instance().stop();
    event_bus_shutdown();
    config_free(cfg);
    log_shutdown();
    return 0;
}
