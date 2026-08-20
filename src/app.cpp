#include "app.h"
/* FTXUI v6.0.0 headers rely on these but don't include them explicitly */
#include <mutex>
#include <condition_variable>
#include <thread>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
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
#include "infra/config.h"
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
#include "plugins/music_sources/netease/netease_api.h"
}

#include "ui/state_store.h"
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
#include "ui/theme.h"
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
    /* ── Special events (C0 control chars) ──
       ftxui routes every byte < 0x20 to Event::Special; Ctrl+/ is
       US (0x1f) or NUL (0x00) depending on the terminal. */
    if (event.input().size() == 1) {
        unsigned char c = (unsigned char)event.input()[0];
        if (c == 0x1f || c == 0x00)
            return "ctrl+/";
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

/* ── Seek accumulation ────────────────────────────── */
/* Press: accumulate. Release (>150ms idle): fire once. */
static int g_seek_accum = 0;
static int g_seek_target = -1;   /* seek destination (sec), -1 = none; cleared when progress reaches target */
static std::chrono::steady_clock::time_point g_last_seek_tp;

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
    char *qr = netease_qr_render(url);
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
    if (netease_qr_key(unikey, sizeof(unikey), qr_url, sizeof(qr_url)) == 0
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
            char *b64 = netease_qr_image(url.c_str());
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
    const char *name = netease_account_name();
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

/* True while the right search box has pushed a nav snapshot for its
   results view; Esc (or a query edit that misses the cache) restores. */
static bool g_top_search_pushed = false;

/* Free all SongInfo strings in a cache vector (for eviction / shutdown) */
static void ns_cache_vec_free(std::vector<SongInfo> &v) {
    for (auto &s : v) song_info_free(&s);
    v.clear();
}

struct LoadedSongs { SongInfo *songs; int count; };

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
        if (netease_search(q.c_str(), 100, 0, &nr) != 0) {
            /* search failed — publish done to clear search+loading */
            event_bus_publish(EV_SEARCH_DONE, NULL, 0);
            return;
        }

        std::vector<SongInfo> vec;
        vec.reserve(nr.count);
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
        netease_search_free(&nr);

        /* Store in cache (transfer ownership), evict oldest if full.
           The evicted vector's SongInfo strings must be freed. */
        if (g_ns_cache.size() >= NS_CACHE_MAX) {
            auto oldest = g_ns_cache.begin();
            ns_cache_vec_free(oldest->second);
            g_ns_cache.erase(oldest);
        }
        g_ns_cache[q] = std::move(vec);

        /* Deep-copy from cache to the event payload — the cache retains
           ownership of its strings. */
        auto &cached = g_ns_cache[q];
        int sc = (int)cached.size();
        LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
        SongInfo *songs = (SongInfo*)calloc((size_t)sc, sizeof(SongInfo));
        for (int i = 0; i < sc; i++) {
            song_info_copy(&songs[i], &cached[i]);
        }
        ld->songs = songs; ld->count = sc;
        if (event_bus_publish(EV_PLAYLIST_LOADED, ld, sizeof(*ld)) != 0) {
            for (int i = 0; i < sc; i++) song_info_free(&songs[i]);
            free(songs); free(ld);
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
    auto it = g_ns_cache.find(q);
    if (it == g_ns_cache.end()) return false;
    if (!g_top_search_pushed) {
        StateStore::instance().nav_push_restore_playlist();
        g_top_search_pushed = true;
    }
    StateStore::instance().set_playlist(it->second, 0);
    StateStore::instance().set_active_panel(1);
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
        st.set_top_left_query("");
        st.set_top_right_query("");
    }
    if (g_top_search_pushed) {
        st.nav_pop();
        g_top_search_pushed = false;
    }
    st.set_top_search_active(false, 0);
}

/* Clear ALL search state — used when the user leaves the search context
   (activates another menu item / local group): the new list must not be
   filtered by the old query and the search nav push must unwind. */
static void clear_search_state(void) {
    StateStore &st = StateStore::instance();
    st.set_top_left_query("");
    st.set_top_right_query("");
    st.set_top_search_active(false, 0);
    st.set_top_search_api(false);
    if (g_top_search_pushed) {
        st.nav_pop();
        g_top_search_pushed = false;
    }
}

/* After a right-box query edit: apply cached results instantly when the
   list is empty; otherwise fall back to the pre-search view so stale
   results don't linger. */
static void restore_search_view(const std::string &q) {
    if (!g_top_search_pushed) return;
    if (!q.empty() && netease_search_apply_cache(q)) return;
    /* cache miss or cleared query — restore the pre-search view */
    NavState snap;
    if (StateStore::instance().nav_peek(snap)) {
        StateStore::instance().set_playlist(snap.playlist, snap.selected_index);
        StateStore::instance().set_active_panel(snap.active_panel);
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
        StateStore::instance().set_top_search_active(true, 1);
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
        });
        StateStore::instance().set_netease_selected(0);
    } else if (type == 301) {
        /* my playlists */
        StateStore::instance().nav_push();
        StateStore::instance().set_loading(true);
        std::thread([]() {
            SongInfo *pl = NULL; int pc = 0;
            int ret = netease_playlists(false, &pl, &pc);
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
    } else if (type == 302 || type == 4) {
        /* liked songs (account page 302 / main menu 4) */
        StateStore::instance().nav_push();
        StateStore::instance().set_loading(true);
        std::thread([]() {
            SongInfo *ms = NULL; int mc = 0;
            int ret = netease_liked_songs(&ms, &mc);
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
            int ok = netease_login_refresh();
            if (ok == 0) {
                const char *name = netease_account_name();
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
        netease_logout();
        StateStore::instance().clear_nav_stack();
        StateStore::instance().set_netease_menu({});
        StateStore::instance().set_music_mode(MusicMode::Netease);
    } else if (!pl_id.empty()) {
        /* entering a playlist's songs from a playlist list — push with
           playlist restore so Esc lands back on the playlist list */
        StateStore::instance().nav_push_restore_playlist();
        StateStore::instance().set_loading(true);
        std::string _pl_id = pl_id;
        std::thread([_pl_id]() {
            SongInfo *songs = NULL; int sc = 0;
            int ret = netease_playlist_songs(_pl_id.c_str(), &songs, &sc);
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

    } else if (type == 0 || type == 1 || type == 5) {
        /* 每日推荐 (0, songs) / 推荐歌单 (1, playlists) / 排行榜 (5, playlists) */
        if (!netease_is_logged_in() && type == 0) {
            start_login();  /* daily recommends need a session */
        } else {
            StateStore::instance().nav_push();
            StateStore::instance().set_loading(true);
            std::thread([type]() {
                SongInfo *songs = NULL; int sc = 0;
                int ret = (type == 5) ? netease_toplist(&songs, &sc)
                                      : netease_menu_songs(type, 30, &songs, &sc);
                LOG_INFO("MENU SONGS: type=%d ret=%d count=%d", type, ret, sc);
                LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
                if (ret == 0 && sc > 0) {
                    ld->songs = songs; ld->count = sc;
                } else {
                    ld->songs = NULL; ld->count = 0;
                    free(songs);
                }
                EventType ev = (type == 1 || type == 5) ? EV_PLAYLIST_LIST_LOADED : EV_PLAYLIST_LOADED;
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
        if (!netease_is_logged_in()) {
            start_login();
        } else {
            StateStore::instance().nav_push();
            StateStore::instance().set_loading(true);
            std::thread([type]() {
                SongInfo *pl = NULL; int pc = 0;
                int ret = netease_playlists(type == 3, &pl, &pc);
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
        if (!netease_is_logged_in()) {
            start_login();
        } else {
            StateStore::instance().nav_push();
            StateStore::instance().set_loading(true);
            std::thread([]() {
                SongInfo *songs = NULL; int sc = 0;
                int ret = netease_liked_songs(&songs, &sc);
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
    if (netease_lyric(song_id, &lyric_buf) == 0 && lyric_buf) {
        ly = lyric_parse(lyric_buf);
        free(lyric_buf);
    }
    LyricLoadResult res = { ly, song_id };
    event_bus_publish(EV_LYRIC_LOADED, &res, sizeof(res));
}

/* In-flight marker (UI thread only): song whose lyrics are being fetched.
   Prevents duplicate spawns from EV_TRACK_CHANGED + EV_PLAYBACK_START
   firing for the same song. */
static char *g_lyric_pending_id = NULL;

static void ev_lyric_loaded(const BusEvent *ev, void *data) {
    (void)data;
    if (ev->data_size != sizeof(LyricLoadResult)) return;
    const LyricLoadResult *res = (const LyricLoadResult*)ev->data;
    auto &store = StateStore::instance();
    const SongInfo &cur = store.state().current_song;
    bool match = res->song_id && cur.id &&
                 strcmp(res->song_id, cur.id) == 0;

    if (g_lyric_pending_id) {
        free(g_lyric_pending_id);
        g_lyric_pending_id = NULL;
    }

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
    if (StateStore::instance().state().playback_state == PlaybackState::Stopped)
        return;
    if (ev->data_size == sizeof(int[3])) {
        int *p = (int*)ev->data;
        double prog = (p[1] > 0) ? (double)p[0] / p[1] : 0.0;
        int tot = (p[2] > 0 && p[1] > 0) ? p[1] / p[2] : 0;
        int cur_ms = (p[2] > 0) ? (int)((long long)p[0] * 1000LL / p[2]) : 0;
        /* Clear seek target once progress is within 1s of the target (seek executed) */
        int seek_dist = cur_ms / 1000 - g_seek_target;
        if (g_seek_target >= 0 && seek_dist >= -1 && seek_dist <= 1) {
            g_seek_target = -1;
            StateStore::instance().set_seek_target_progress(0.0f);
            /* Progress caught up — fall through to update */
        }
        if (g_seek_target < 0) {
            /* No pending seek: normal progress update.
               Skip when seek is pending but playback hasn't caught up
               yet — prevents stale EV_PROGRESS_UPDATE (sent before the
               playback thread processed CMD_SEEK) from overwriting the
               already-correct progress set by consume_seek(). */
            StateStore::instance().set_progress_ms(prog, cur_ms, tot);
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
    g_seek_target = target;
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
        if (g_lyric_pending_id &&
            strcmp(g_lyric_pending_id, song.id) == 0)
            return;  /* already loading this song's lyrics */
        char *dup = strdup(song.id);
        if (!dup || !g_thread_pool) { free(dup); return; }
        threadpool_submit(g_thread_pool, lyric_load_worker, dup);
        free(g_lyric_pending_id);
        g_lyric_pending_id = strdup(song.id);
        /* Cover is now loaded lazily when entering lyric mode */
        return;
    }
}

static void ev_track_changed(const BusEvent *ev, void *data) {
    (void)ev; (void)data;
    load_lyrics_for_current_song();
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
static const char *xdg_dir(const char *env, const char *sub) {
    const char *d = getenv_utf8(env);
    static char buf[1024];
#ifndef _WIN32
    if (d && d[0]) {
        snprintf(buf, sizeof(buf), "%s/netune/%s", d, sub ? sub : "");
    } else {
        const char *home = getenv_utf8("HOME");
        if (!home) home = "/tmp";
        const char *prefix = strstr(env, "CONFIG") ? ".config" : ".cache";
        snprintf(buf, sizeof(buf), "%s/%s/netune/%s", home, prefix, sub ? sub : "");
    }
#else
    /* Windows: map XDG_*_HOME to APPDATA / LOCALAPPDATA */
    const char *win_env = NULL;
    if (strstr(env, "CACHE"))
        win_env = "LOCALAPPDATA";
    else if (strstr(env, "CONFIG"))
        win_env = "APPDATA";
    if (win_env) d = getenv_utf8(win_env);
    if (d && d[0]) {
        snprintf(buf, sizeof(buf), "%s\\netune\\%s", d, sub ? sub : "");
    } else {
        const char *home = getenv_utf8("USERPROFILE");
        if (!home) home = "C:\\";
        const char *prefix = strstr(env, "CONFIG") ? ".config" : ".cache";
        snprintf(buf, sizeof(buf), "%s\\%s\\netune\\%s", home, prefix, sub ? sub : "");
    }
#endif
    return buf;
}

static void ensure_dir(const char *filepath) {
    /* mkdir -p the parent directory of filepath */
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    /* find last separator (handles both / and \) */
    char *last_slash = strrchr(tmp, '/');
    char *last_bslash = strrchr(tmp, '\\');
    char *last = (last_bslash && last_bslash > last_slash) ? last_bslash : last_slash;
    if (!last) return;
    if (last == tmp) return;  /* root dir, nothing to do */
    char sep_chr = *last;
    *last = 0;  /* strip file name (or trailing separator) */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = 0;
            mkdir_utf8(tmp);
            *p = saved;
        }
    }
    if (tmp[0]) {
        mkdir_utf8(tmp);
    }
    (void)sep_chr;
}

/* ── XDG data root helper ─────────────────────────── */
/* Returns the canonical data root where ALL runtime-editable
   resources live: XDG_CONFIG_HOME/netune/data/
   Cache and log are intentionally NOT under here — they stay
   under XDG_CACHE_HOME. */
static const char *xdg_data_root(void) {
    static char buf[1024];
#ifndef _WIN32
    const char *d = getenv_utf8("XDG_CONFIG_HOME");
    if (d && d[0]) {
        snprintf(buf, sizeof(buf), "%s/netune/data", d);
    } else {
        const char *home = getenv_utf8("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, sizeof(buf), "%s/.config/netune/data", home);
    }
#else
    /* Windows: use APPDATA (same mapping as XDG_CONFIG_HOME) */
    const char *d = getenv_utf8("APPDATA");
    if (d && d[0]) {
        snprintf(buf, sizeof(buf), "%s\\netune\\data", d);
    } else {
        const char *home = getenv_utf8("USERPROFILE");
        if (!home) home = "C:\\";
        snprintf(buf, sizeof(buf), "%s\\.config\\netune\\data", home);
    }
#endif
    return buf;
}

/* ── Default data tree content ─────────────────────── */
/* These embedded blobs let the app rebuild a complete default
   data/ tree on startup — no scanning, no fallback lookups. */

static const char *DEFAULT_CONFIG_JSON =
    "{\n"
    "  \"version\": \"1.0\",\n"
    "  \"audio\": { \"backend\": \"auto\", \"volume\": 80 },\n"
    "  \"playback\": { \"loop_mode\": 0, \"seek_step_sec\": 5 },\n"
    "  \"ui\": { \"theme\": \"default\", \"layout\": \"default\", \"keybindings\": \"default\" },\n"
    "  \"music_sources\": {\n"
    "    \"local\": { \"enabled\": true, \"dirs\": [] },\n"
    "    \"netease\": { \"enabled\": true }\n"
    "  }\n"
    "}\n";

static const char *DEFAULT_LAYOUT_YAML =
    "layout:\n"
    "  type: \"vertical\"\n"
    "  children:\n"
    "    - component: \"top_bar\"\n"
    "      height: 1\n"
    "    - type: \"horizontal\"\n"
    "      flex: 1\n"
    "      children:\n"
    "        - component: \"group_list\"\n"
    "          width: 20\n"
    "        - component: \"song_list\"\n"
    "          flex: 1\n"
    "    - component: \"status_bar\"\n"
    "      height: 2\n";

static const char *DEFAULT_KEYBINDINGS_YAML =
    "keybindings:\n"
    "  move_down:     [\"j\", \"down\"]\n"
    "  move_up:       [\"k\", \"up\"]\n"
    "  panel_switch:  [\"tab\"]\n"
    "  play_pause:    [\"space\"]\n"
    "  play_select:   [\"enter\"]\n"
    "  next_track:    [\"n\"]\n"
    "  prev_track:    [\"p\"]\n"
    "  seek_forward:   [\"right\"]\n"
    "  seek_backward:  [\"left\"]\n"
    "  volume_up:     [\"+\", \"=\"]\n"
    "  volume_down:   [\"-\"]\n"
    "  open_search:   [\"ctrl+/\"]\n"
    "  stop:          [\"s\"]\n"
    "  toggle_mute:   [\"m\"]\n"
    "  cycle_loop:    [\"r\"]\n"
    "  toggle_lyrics: [\"l\"]\n"
    "  show_help:     [\"?\", \"escape\"]\n"
    "  quit:          [\"q\"]\n";

static const char *DEFAULT_THEME_DEFAULT_YAML =
    "name: \"Tokyo Night\"\n"
    "colors:\n"
    "  bg: \"#1a1b26\"\n"
    "  fg: \"#c0caf5\"\n"
    "  accent: \"#7aa2f7\"\n"
    "  accent_bg: \"#33467c\"\n"
    "  muted: \"#565f89\"\n"
    "  border: \"#292e42\"\n"
    "  success: \"#9ece6a\"\n"
    "  warning: \"#e0af68\"\n"
    "  error: \"#f7768e\"\n"
    "  overlay_bg: \"#16161e\"\n";

static const char *DEFAULT_THEME_CATPPUCCIN_YAML =
    "name: \"Catppuccin Mocha\"\n"
    "colors:\n"
    "  bg: \"#1e1e2e\"\n"
    "  fg: \"#cdd6f4\"\n"
    "  accent: \"#89b4fa\"\n";

static const char *DEFAULT_THEME_DRACULA_YAML =
    "name: \"Dracula\"\n"
    "colors:\n"
    "  bg: \"#282a36\"\n"
    "  fg: \"#f8f8f2\"\n"
    "  accent: \"#bd93f9\"\n";

static const char *DEFAULT_THEME_NETEASE_DARK_YAML =
    "name: \"Netease Dark\"\n"
    "colors:\n"
    "  bg: \"#1a1a2e\"\n"
    "  fg: \"#c8c8dc\"\n"
    "  accent: \"#e3322d\"\n";

static const char *DEFAULT_THEME_NETEASE_LIGHT_YAML =
    "name: \"Netease Light\"\n"
    "colors:\n"
    "  bg: \"#f5f5f5\"\n"
    "  fg: \"#333333\"\n"
    "  accent: \"#d43c33\"\n";

/* ── Ensure the full default data tree exists ──────── */
/* On startup, walks the canonical data root and creates
   whatever is missing — directories AND default file
   contents. Existing files are NEVER overwritten.
   No external scanning or fallback lookup is performed. */
static void ensure_default_data_tree(void) {
    const char *root = xdg_data_root();

    /* Ensure the root directory exists */
    ensure_dir(root);

    /* Helper: create a default file (with parent dirs) if it
       does not already exist. Never touches existing files. */
    auto ensure_file = [&](const char *rel, const char *content) {
        char path[2048];
        snprintf(path, sizeof(path), "%s" PATH_SEP "%s", root, rel);
        if (access_utf8(path, F_OK) == 0) return;  /* already there */
        ensure_dir(path);
        FILE *f = fopen_utf8(path, "w");
        if (f) {
            fputs(content, f);
            fclose(f);
            LOG_INFO("Created default data file: %s", path);
        } else {
            LOG_WARN("Failed to create default data file: %s", path);
        }
    };

    /* config.json at the data root */
    ensure_file("config.json", DEFAULT_CONFIG_JSON);
    /* layouts */
    ensure_file("layouts/default.yaml", DEFAULT_LAYOUT_YAML);
    /* keybindings */
    ensure_file("keybindings/default.yaml", DEFAULT_KEYBINDINGS_YAML);
    /* themes — all bundled defaults */
    ensure_file("themes/default.yaml",       DEFAULT_THEME_DEFAULT_YAML);
    ensure_file("themes/catppuccin.yaml",    DEFAULT_THEME_CATPPUCCIN_YAML);
    ensure_file("themes/dracula.yaml",       DEFAULT_THEME_DRACULA_YAML);
    ensure_file("themes/netease_dark.yaml",  DEFAULT_THEME_NETEASE_DARK_YAML);
    ensure_file("themes/netease_light.yaml", DEFAULT_THEME_NETEASE_LIGHT_YAML);
}

int run_app(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Netune v2.0.0 — Terminal music player\nUsage: %s [config.json]\n", argv[0]);
        return 0;
    }

    /* ── Log ────────────────────────────────────────── */
    const char *log_path = xdg_dir("XDG_CACHE_HOME", "netune.log");
    ensure_dir(log_path);
    log_init(log_path);
    LOG_INFO("Netune v2.0.0 starting");

    /* Probe terminal cell size (before FTXUI takes over the terminal)
       so the character cover renderer can keep aspect ratio on any font */
    cover_cell_probe();

    /* ── Ensure default data tree exists (XDG_CONFIG_HOME/netune/data/) ── */
    /* Rebuilds config.json / themes / layouts / keybindings if missing.
       No scanning, no fallback lookups elsewhere. */
    ensure_default_data_tree();

    /* ── Config (under data/) ── */
    char cfg_buf[2048];
    snprintf(cfg_buf, sizeof(cfg_buf), "%s" PATH_SEP "config.json", xdg_data_root());
    Config *cfg = config_load(cfg_buf);
    if (!cfg) LOG_WARN("No config loaded, using defaults");
    config_set_global(cfg);

    /* ── Cache (XDG_CACHE_HOME) ─────────────────────── */
    const char *cache_dir = xdg_dir("XDG_CACHE_HOME", NULL);
    ensure_dir(cache_dir);
    mkdir_utf8(cache_dir);
    cache_init(cache_dir);
    search_manager_init();

    event_bus_init();

    g_thread_pool = threadpool_create(8);
    if (!g_thread_pool) LOG_WARN("Failed to create thread pool, cover art will not load");

    /* Default templates for themes/keybindings/layouts are created by
       ensure_default_data_tree() above — nothing else to do here. */

        event_bus_subscribe(EV_PROGRESS_UPDATE,   ev_progress, NULL);
    event_bus_subscribe(EV_PLAYBACK_START,    ev_playback_start, NULL);
    event_bus_subscribe(EV_PLAYBACK_PAUSE,    ev_playback_pause, NULL);
    event_bus_subscribe(EV_PLAYBACK_RESUME,   ev_playback_resume, NULL);
    event_bus_subscribe(EV_PLAYBACK_STOP,     ev_playback_stop, NULL);
    event_bus_subscribe(EV_PLAYBACK_FINISH,   ev_playback_finish, NULL);
    event_bus_subscribe(EV_MPRIS_COMMAND,     ev_mpris_command, NULL);
        event_bus_subscribe(EV_PLAYLIST_LOADED, ev_playlist_loaded, NULL);
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
                 xdg_data_root(), name);
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
                 xdg_data_root(), name);
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

    /* auto-scan */
    {
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
        /* No dirs configured — no fallback scan */
        (void)0;
        std::vector<SongGroup> groups;
        for (auto &dir : scan_dirs) {
            SearchResult result;
            memset(&result, 0, sizeof(result));
            if (music_source_search("local", dir.c_str(), 0, 0, &result) != 0 || result.count <= 0) {
                if (result.songs) free(result.songs);
                continue;
            }
            SongGroup g;
            const char *slash_fwd = strrchr(dir.c_str(), '/');
            const char *slash_back = strrchr(dir.c_str(), '\\');
            const char *last_sep = (slash_back && (!slash_fwd || slash_back > slash_fwd))
                                   ? slash_back : slash_fwd;
            g.name = last_sep ? last_sep + 1 : dir.c_str();
            for (int j = 0; j < result.count; j++) {
                SongInfo copy;
                song_info_copy(&copy, &result.songs[j]);
                g.songs.push_back(copy);
                song_info_free(&result.songs[j]);
            }
            free(result.songs);
            groups.push_back(std::move(g));
        }
        if (!groups.empty()) {
            StateStore::instance().set_groups(groups);
            /* sync first group's paths to backend */
            {
                const auto &st = StateStore::instance().state();
                std::vector<const char*> paths;
                for (auto &s : st.groups[0].songs) paths.push_back(s.id);
            }
            LOG_INFO("Scanned %zu groups", groups.size());
        } else {
            LOG_WARN("No music files found");
        }
    }

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
            int ms = (st.playback_state == PlaybackState::Playing || st.loading || st.cover_loading || st.action_sheet_open)
                      ? 33 : 200;
#else
            int ms = (st.playback_state == PlaybackState::Playing || st.loading || st.cover_loading || st.action_sheet_open)
                      ? 16 : 200;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            screen.RequestAnimationFrame();
        }
    });

    auto &state = StateStore::instance();

    auto consume_seek = [&]() {
        auto now = std::chrono::steady_clock::now();
        if (g_seek_accum == 0) return;
        if (now - g_last_seek_tp < std::chrono::milliseconds(150))
            return;
        const AppState &st = state.state();
        if (st.playback_state != PlaybackState::Stopped && st.total_time_sec > 0) {
            int target = st.current_time_sec + g_seek_accum;
            if (target > st.total_time_sec) target = st.total_time_sec;
            if (target < 0) target = 0;
            /* Skip seek if already at target — prevents rapid
               seek-to-0 loops when holding past the start */
            if (target != st.current_time_sec) {
                event_bus_publish(EV_BUFFERING_UPDATE, &target, sizeof(target));
                g_seek_target = target;
                state.set_seek_target_progress((float)target / st.total_time_sec);
                /* Immediately sync progress so the gauge doesn't fall back
                   to the old s.progress value when seek_target_progress is
                   0.0f (target=0 → 0/total → 0.0f, and > 0.0f is false). */
                state.set_progress((double)target / st.total_time_sec, target, st.total_time_sec);
            }
        }
        g_seek_accum = 0;
        state.set_seek_indicator(0);
    };

    auto component = Renderer([&]() -> Element {
        event_bus_poll();
#ifndef _WIN32
        mpris_sync(&state.state());
#endif
        consume_seek();
        const AppState &s = state.state();

        state.set_song_panel_width(screen.dimx() - 29);
        state.set_screen_height(screen.dimy());
        state.set_top_row_width(screen.dimx());

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
            int rc = netease_qr_poll(g_login_unikey.c_str());
            LOG_INFO("LOGIN POLL: rc=%d", rc);
            if (rc == 0) {
                /* 803: authorized, login successful */
                login_done_at = std::chrono::steady_clock::now();
                StateStore::instance().set_login_state(3,
                    netease_account_name() ? netease_account_name() : "Logged in", "");
                update_login_menu();
                /* Data-loading phase: warm up the user's playlists in
                   the background; the success overlay closes when this
                   finishes (or after the 10s timeout above). */
                std::thread([]() {
                    SongInfo *pl = NULL; int pc = 0;
                    netease_playlists(false, &pl, &pc);
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

        return main;
    });

    component |= CatchEvent([&](ftxui::Event event) -> bool {
        /* ── Action sheet (Ctrl+X) modal handling ── */
        {
            const AppState &as = state.state();
            if (as.action_sheet_open) {
                std::string k;
                if (event.is_character()) k = event.character();
                else k = event_to_key_name(event);
                if (k == "enter" || k == "\r") {
                    const auto &item = as.playlist[as.selected_index];
                    bool is_pl = item.is_playlist;
                    std::string id = item.id ? item.id : "";
                    bool active = as.action_sheet_active == 1;
                    if (id.empty() || as.action_sheet_active < 0) return true;
                    std::thread([id, is_pl, active]() {
                        int rv = is_pl
                            ? netease_subscribe_playlist(id.c_str(), !active)
                            : netease_like_song(id.c_str(), !active);
                        LOG_INFO("ACTION SHEET: %s %s toggle->%d = %d",
                                 is_pl ? "subscribe" : "like", id.c_str(), !active, rv);
                        if (rv == 0)
                            StateStore::instance().set_action_sheet_active(active ? 0 : 1);
                    }).detach();
                    return true;
                }
                if (k == "escape") {
                    state.set_action_sheet(false, 0);
                    return true;
                }
                return true;  /* swallow all keys while open */
            }
        }

        /* ── Non-seek key → discard pending seek ── */
        if (g_seek_accum != 0 && event != ftxui::Event::ArrowLeft && event != ftxui::Event::ArrowRight) {
            g_seek_accum = 0;
            g_seek_target = -1;
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
            int side = cur.top_search_side;

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
            if (ev_key == "tab") {
                /* move editing focus to the other box */
                StateStore::instance().set_top_search_active(true, side ? 0 : 1);
                return true;
            }
            if (ev_key == "up" || ev_key == "down") {
                /* Cursor model: the search box and the list are one
                   continuous cursor. Down leaves the box and lands on
                   the FIRST visible item (first match when filtering) —
                   a deterministic target, never a hidden selection. */
                StateStore::instance().set_top_search_active(false, 0);
                const auto &st2 = StateStore::instance().state();
                if (side == 1) {
                    if (!cur.top_search_api && !cur.top_right_query.empty() &&
                        !st2.playlist.empty()) {
                        int m = next_match(st2.playlist, -1, 1,
                                           cur.top_right_query);
                        StateStore::instance().set_selected_index(m >= 0 ? m : 0);
                    } else {
                        StateStore::instance().set_selected_index(0);
                    }
                } else if (cur.music_mode == MusicMode::Local) {
                    /* first matching group when the left box filters */
                    if (!cur.top_left_query.empty() && !st2.groups.empty()) {
                        int m = -1;
                        for (size_t i = 0; i < st2.groups.size(); i++) {
                            if (str_icontains(st2.groups[i].name, cur.top_left_query)) {
                                m = (int)i; break;
                            }
                        }
                        StateStore::instance().set_group_index(m >= 0 ? m : 0);
                    } else {
                        StateStore::instance().set_group_index(0);
                    }
                } else {
                    /* first matching netease menu item when filtered */
                    if (!cur.top_left_query.empty() && !st2.netease_menu.empty()) {
                        int m = -1;
                        for (size_t i = 0; i < st2.netease_menu.size(); i++) {
                            if (str_icontains(st2.netease_menu[i].name, cur.top_left_query)) {
                                m = (int)i; break;
                            }
                        }
                        StateStore::instance().set_netease_selected(m >= 0 ? m : 0);
                    } else {
                        StateStore::instance().set_netease_selected(0);
                    }
                }
                return true;
            }
            if (ev_key == "enter" || ev_key == "\r") {
                if (side == 0) {
                    /* committing the left filter must first unwind any
                       pending right-box search push so the nav stack
                       stays balanced */
                    clear_search_state();
                    if (cur.music_mode == MusicMode::Local) {
                        /* activate the matching local group (netease entry is
                           excluded from local search — strict separation) */
                        const std::string &q = cur.top_left_query;
                        int target = -2;
                        if (q.empty()) {
                            target = cur.group_index;
                        } else {
                            for (size_t i = 0; i < cur.groups.size(); i++) {
                                if (str_icontains(cur.groups[i].name, q)) {
                                    target = (int)i;
                                    break;
                                }
                            }
                        }
                        if (target < -1) return true;  /* no match — just close */
                        if (target < 0) {
                            /* netease entry: switch to netease mode */
                            StateStore::instance().set_music_mode(MusicMode::Netease);
                            StateStore::instance().set_active_panel(0);
                            StateStore::instance().set_group_index(-1);
                        } else {
                            StateStore::instance().set_group_index(target);
                            StateStore::instance().set_active_panel(1);
                        }
                    } else {
                        /* activate the first menu item matching the filter */
                        const std::string &q = cur.top_left_query;
                        int target = -1;
                        if (q.empty()) {
                            target = cur.netease_selected;
                        } else {
                            for (size_t i = 0; i < cur.netease_menu.size(); i++) {
                                if (str_icontains(cur.netease_menu[i].name, q)) {
                                    target = (int)i;
                                    break;
                                }
                            }
                        }
                        if (target >= 0) {
                            StateStore::instance().set_netease_selected(target);
                            activate_netease_menu_item(target);
                        }
                    }
                } else {
                    /* right box:
                       - normal (non-netease) mode: Enter does nothing —
                         the box stays selected & editable
                       - netease mode: Enter submits the API search and
                         leaves the box so the results are interactive */
                    if (cur.top_search_api &&
                        !cur.top_right_query.empty()) {
                        if (!netease_search_apply_cache(cur.top_right_query)) {
                            do_netease_search(cur.top_right_query.c_str(),
                                              !g_top_search_pushed);
                            g_top_search_pushed = true;
                        }
                        StateStore::instance().set_top_search_active(false, 0);
                        StateStore::instance().set_active_panel(1);
                    }
                }
                return true;
            }
            if (ev_key == "backspace") {
                /* remove last char (UTF-8 safe) */
                std::string q = (side == 0) ? cur.top_left_query
                                             : cur.top_right_query;
                if (!q.empty()) {
                    int n = (int)q.size() - 1;
                    while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80) n--;
                    q.resize((size_t)n);
                }
                if (side == 0) {
                    StateStore::instance().set_top_left_query(q);
                } else {
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
                std::string q = ((side == 0) ? cur.top_left_query
                                             : cur.top_right_query) + ch;
                if (side == 0) {
                    StateStore::instance().set_top_left_query(q);
                } else {
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
            if (!cur.top_left_query.empty() ||
                !cur.top_right_query.empty()) {
                StateStore::instance().set_top_search_active(true,
                    !cur.top_right_query.empty() ? 1 : 0);
                return true;
            }
            if (!cur.nav_stack.empty()) {
                StateStore::instance().nav_pop();
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
                    /* local groups: skip non-matching when filtered */
                    int n = (int)cur.groups.size();
                    if (n <= 0) return true;
                    int idx = cur.group_index;
                    const std::string &q = cur.top_left_query;
                    int target = -1;
                    for (int k = 0; k < n; k++) {
                        idx = (idx + 1 >= n) ? 0 : idx + 1;
                        if (q.empty() || str_icontains(cur.groups[idx].name, q)) {
                            target = idx;
                            break;
                        }
                    }
                    if (target < 0) return true;
                    StateStore::instance().set_group_index(target);
                    if (target >= 0 && target < n) {
                        std::vector<const char*> paths;
                        for (auto &s : cur.groups[target].songs) paths.push_back(s.id);
                    }
                } else {
                    /* netease menu: skip non-matching items when the
                       left box filters */
                    int next = cur.netease_selected;
                    int n = (int)cur.netease_menu.size();
                    const std::string &q = cur.top_left_query;
                    for (int k = 0; k < n; k++) {
                        next = (next + 1 >= n) ? 0 : next + 1;
                        if (q.empty() ||
                            str_icontains(cur.netease_menu[next].name, q)) {
                            StateStore::instance().set_netease_selected(next);
                            break;
                        }
                    }
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
                    StateStore::instance().set_top_search_active(true, 1);
                    return true;
                }
            }
            if (cur.active_panel == 0) {
                if (cur.music_mode == MusicMode::Local) {
                    /* local groups: skip non-matching when filtered */
                    int n = (int)cur.groups.size();
                    if (n <= 0) return true;
                    int idx = cur.group_index;
                    const std::string &q = cur.top_left_query;
                    int target = -1;
                    for (int k = 0; k < n; k++) {
                        idx = (idx - 1 < 0) ? n - 1 : idx - 1;
                        if (q.empty() || str_icontains(cur.groups[idx].name, q)) {
                            target = idx;
                            break;
                        }
                    }
                    if (target < 0) return true;
                    if (target == cur.group_index && !q.empty()) {
                        /* wrapped: at the top matching group — re-enter box */
                        StateStore::instance().set_top_search_active(true, 0);
                        return true;
                    }
                    StateStore::instance().set_group_index(target);
                    if (target >= 0 && target < n) {
                        std::vector<const char*> paths;
                        for (auto &s : cur.groups[target].songs) paths.push_back(s.id);
                    }
                } else {
                    /* netease menu: skip non-matching items; Up from the
                       top matching item re-enters the left search box */
                    int prev = cur.netease_selected;
                    int n = (int)cur.netease_menu.size();
                    const std::string &q = cur.top_left_query;
                    int target = -1;
                    for (int k = 0; k < n; k++) {
                        prev = (prev - 1 < 0) ? n - 1 : prev - 1;
                        if (q.empty() ||
                            str_icontains(cur.netease_menu[prev].name, q)) {
                            target = prev;
                            break;
                        }
                    }
                    if (target < 0) return true;
                    if (target == cur.netease_selected && !q.empty()) {
                        /* wrapped around: we are at the top matching item */
                        StateStore::instance().set_top_search_active(true, 0);
                        return true;
                    }
                    StateStore::instance().set_netease_selected(target);
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
                    /* Switch back to Local mode */
                    StateStore::instance().set_music_mode(MusicMode::Local);
                    StateStore::instance().set_active_panel(0);
                    StateStore::instance().set_group_index(0);
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
                    StateStore::instance().set_loading(true);
                    std::string _pl_id = song.id ? song.id : "";
                    std::thread([_pl_id]() {
                        SongInfo *songs = NULL; int sc = 0;
                        int ret = netease_playlist_songs(_pl_id.c_str(), &songs, &sc);
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
                g_seek_accum += config_get_int(config_global(), "playback.seek_step_sec", 5);
                g_last_seek_tp = std::chrono::steady_clock::now();
                state.set_seek_indicator(g_seek_accum);
            }
            return true;

        case Action::SeekBackward:
            if (cur.playback_state != PlaybackState::Stopped && cur.total_time_sec > 0) {
                g_seek_accum -= config_get_int(config_global(), "playback.seek_step_sec", 5);
                g_last_seek_tp = std::chrono::steady_clock::now();
                state.set_seek_indicator(g_seek_accum);
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
                StateStore::instance().set_top_search_active(true,
                                                              cur.active_panel);
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
                const auto &item = cur.playlist[cur.selected_index];
                bool is_pl = item.is_playlist;
                std::string id = item.id ? item.id : "";
                if (!id.empty()) {
                    std::thread([id, is_pl]() {
                        int active = -1;
                        if (is_pl) {
                            SongInfo *pls = NULL; int pc = 0;
                            if (netease_playlists(true, &pls, &pc) == 0) {
                                for (int i = 0; i < pc; i++) {
                                    if (pls[i].id && strcmp(pls[i].id, id.c_str()) == 0) {
                                        active = 1; break;
                                    }
                                }
                                if (active < 0) active = 0;
                                for (int i = 0; i < pc; i++) song_info_free(&pls[i]);
                                free(pls);
                            }
                        } else {
                            bool liked = false;
                            if (netease_liked_check(id.c_str(), &liked) == 0)
                                active = liked ? 1 : 0;
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
                        int spec_rows = screen.dimy() / 12;
                        if (spec_rows < 2) spec_rows = 2;
                        if (spec_rows > 4) spec_rows = 4;
                        int panel_h = screen.dimy() - 1 - spec_rows - 2;
                        int row0 = 2;
                        if (panel_h > dh)
                            row0 += (panel_h - dh) / 2;
                        /* The cover is centered inside its cw-column slot,
                           mirroring the character renderer / placeholder */
                        int col0 = 1 + cover_left_margin(st) + (cw - dw) / 2;

                        auto now = std::chrono::steady_clock::now();
                        bool geometry_changed =
                            !gfx_active ||
                            screen.dimx() != last_dimx ||
                            screen.dimy() != last_dimy ||
                            cw != last_cw || dw != last_dw || dh != last_dh ||
                            st.cover.stamp != last_stamp;
                        bool due =
                            (now - gfx_last_place) >=
                            std::chrono::milliseconds(500);
                        if (geometry_changed || due) {
                            gfx_last_place = now;
                            last_dimx = screen.dimx();
                            last_dimy = screen.dimy();
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
                    int col0 = 1 + (screen.dimx() - cols) / 2;
                    if (col0 < 1) col0 = 1;
                    auto now = std::chrono::steady_clock::now();
                    bool changed = !qr_gfx_placed ||
                                   screen.dimx() != qr_ldx ||
                                   screen.dimy() != qr_ldy ||
                                   rows != qr_lrows ||
                                   g_login_qr.stamp != qr_lstamp;
                    bool due = (now - qr_gfx_last) >=
                               std::chrono::milliseconds(500);
                    if (changed || due) {
                        qr_gfx_last = now;
                        qr_ldx = screen.dimx();
                        qr_ldy = screen.dimy();
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
    event_bus_shutdown();
    config_free(cfg);
    log_shutdown();
    return 0;
}
