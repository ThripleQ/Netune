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
        std::string qr = gen_qr(qr_url);
        StateStore::instance().set_login_state(2,
            "Scan with Netease Music App", qr);
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
        StateStore::instance().nav_push();
        g_top_search_pushed = true;
    }
    StateStore::instance().set_playlist(it->second, 0);
    StateStore::instance().set_active_panel(1);
    return true;
}

/* Close the top search row and restore everything: clear both boxes'
   queries (lists un-filter) and pop the nav snapshot taken for netease
   search results. */
static void close_top_search(void) {
    StateStore &st = StateStore::instance();
    st.set_top_left_query("");
    st.set_top_right_query("");
    if (g_top_search_pushed) {
        st.nav_pop();
        g_top_search_pushed = false;
    }
    st.set_top_search_active(false, 0);
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
        /* netease "search" menu entry — focus the top right search box */
        StateStore::instance().set_top_search_active(true, 1);
    } else if (!pl_id.empty()) {
        StateStore::instance().nav_push();
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

    } else if (type >= 0 && type <= 1) {
        StateStore::instance().nav_push();
        StateStore::instance().set_loading(true);
        int _type = type;
        std::thread([_type]() {
            SongInfo *ms = NULL; int mc = 0;
            int ret = netease_menu_songs(_type, 200, &ms, &mc);
            LoadedSongs *ld = (LoadedSongs*)malloc(sizeof(LoadedSongs));
            if (ret == 0 && mc > 0) {
                ld->songs = ms; ld->count = mc;
            } else {
                ld->songs = NULL; ld->count = 0;
                free(ms);
            }
            if (event_bus_publish(EV_MENU_LOADED, ld, sizeof(*ld)) != 0) {
                if (ld->songs) {
                    for (int i = 0; i < ld->count; i++)
                        song_info_free(&ld->songs[i]);
                    free(ld->songs);
                }
                free(ld);
            }
        }).detach();
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
    CoverData cd = {NULL, 0, 0, 0};
    if (url && cover_load(url, &cd) == 0)
        event_bus_publish(EV_COVER_LOADED, &cd, sizeof(CoverData));
    free(url);
}

/* ── Start cover download (shows spinner, clears old cover) ──── */
static void start_cover_download(const char *url) {
    if (!url || !url[0]) return;
    CoverData empty = {NULL, 0, 0, 0};
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
    if (ld->count <= 0 || !ld->songs) {
        /* ev->data freed by event_bus_poll, do NOT free(ld) */
        return;
    }
    std::vector<NeteaseMenuItem> items;
    items.push_back({"<< \u8FD4\u56DE", -1, ""});
    for (int i = 0; i < ld->count; i++) {
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "%s", ld->songs[i].id);
        items.push_back({ld->songs[i].title, 1000, id_buf});
        song_info_free(&ld->songs[i]);
    }
    free(ld->songs);
    /* Note: ev->data is freed by event_bus_poll, do NOT free(ld) */
    StateStore::instance().set_netease_menu(items);
    StateStore::instance().set_netease_selected(0);
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
    CoverData empty = {NULL, 0, 0, 0};
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
    "  open_search:   [\"/\"]\n"
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
            int ms = (st.playback_state == PlaybackState::Playing || st.loading || st.cover_loading)
                      ? 16 : 200;
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
        state.set_top_row_width(screen.dimx());

        /* Login polling: every ~2s while waiting for QR scan;
           auto-close 2s after successful login */
        if (s.login_state == 3) {
            static int close_tick = 0;
            if (++close_tick >= 125) {
                close_tick = 0;
                StateStore::instance().set_login_state(0, "", "");
            }
        }
        if (s.login_state == 2 && ++g_login_poll_tick % 125 == 0) {
            int rc = netease_qr_poll(g_login_unikey.c_str());
            LOG_INFO("LOGIN POLL: rc=%d", rc);
            if (rc == 0) {
                /* 803: authorized, login successful */
                StateStore::instance().set_login_state(3,
                    netease_account_name() ? netease_account_name() : "Logged in", "");
                update_login_menu();
            } else if (rc == 2) {
                /* 800: expired — restart */
                g_login_unikey.clear();
                start_login();
            } else if (rc == 3) {
                /* 802: scanned, waiting for phone confirm */
                StateStore::instance().set_login_state(2,
                    "Scanned. Confirm in Netease Music App...", s.login_qr);
            }
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

        return main;
    });

    component |= CatchEvent([&](ftxui::Event event) -> bool {
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
            ev_key = event.character();
            if (ev_key == " ") ev_key = "space";
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
                /* close search and restore full views */
                close_top_search();
                return true;
            }
            if (ev_key == "tab") {
                /* move editing focus to the other box */
                StateStore::instance().set_top_search_active(true, side ? 0 : 1);
                return true;
            }
            if (ev_key == "up" || ev_key == "down") {
                int step = (ev_key == "up") ? -1 : 1;
                if (side == 0) {
                    const std::string &q = cur.top_left_query;
                    if (cur.music_mode == MusicMode::Local) {
                        /* entries: -1 = netease entry (only when unfiltered),
                           0..n-1 = groups */
                        int n = (int)cur.groups.size();
                        if (n <= 0) return true;
                        int idx = cur.group_index;
                        auto matches = [&](int i) {
                            if (i < 0) return q.empty();
                            if (i >= (int)cur.groups.size()) return false;
                            return q.empty() || str_icontains(cur.groups[i].name, q);
                        };
                        for (int k = 0; k < n + 2; k++) {
                            idx += step;
                            if (idx < -1) idx = n - 1;
                            if (idx > n - 1) idx = -1;
                            if (matches(idx)) {
                                StateStore::instance().set_group_index(idx);
                                break;
                            }
                        }
                    } else {
                        /* move within filtered menu items */
                        int n = (int)cur.netease_menu.size();
                        if (n <= 0) return true;
                        int idx = cur.netease_selected;
                        for (int k = 0; k < n; k++) {
                            idx += step;
                            if (idx < 0) idx = n - 1;
                            if (idx >= n) idx = 0;
                            if (q.empty() || str_icontains(cur.netease_menu[idx].name, q)) {
                                StateStore::instance().set_netease_selected(idx);
                                break;
                            }
                        }
                    }
                } else {
                    /* move within filtered playlist */
                    const std::string &q = cur.top_right_query;
                    int n = (int)cur.playlist.size();
                    if (n <= 0) return true;
                    int idx = cur.selected_index;
                    if (q.empty()) {
                        idx += step;
                        if (idx < 0) idx = n - 1;
                        if (idx >= n) idx = 0;
                        StateStore::instance().set_selected_index(idx);
                    } else {
                        for (int k = 0; k < n; k++) {
                            idx += step;
                            if (idx < 0) idx = n - 1;
                            if (idx >= n) idx = 0;
                            const auto &song = cur.playlist[idx];
                            std::string h;
                            if (song.title) h += song.title;
                            if (song.artist) h += std::string(" ") + song.artist;
                            if (str_icontains(h, q)) {
                                StateStore::instance().set_selected_index(idx);
                                break;
                            }
                        }
                    }
                }
                return true;
            }
            if (ev_key == "enter" || ev_key == "\r") {
                if (side == 0) {
                    StateStore::instance().set_top_search_active(false, 0);
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
                    /* right box: filter mode plays selected */
                    if (!cur.playlist.empty()) {
                        StateStore::instance().set_top_search_active(false, 1);
                        int idx = cur.selected_index;
                        if (idx >= 0 && idx < (int)cur.playlist.size())
                            play_from_playlist(idx);
                    } else if (!cur.top_right_query.empty() &&
                               cur.music_mode == MusicMode::Netease) {
                        /* netease API search (cache hit → instant) */
                        if (!netease_search_apply_cache(cur.top_right_query)) {
                            do_netease_search(cur.top_right_query.c_str(),
                                              !g_top_search_pushed);
                            g_top_search_pushed = true;
                        }
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
                    restore_search_view(q);
                }
                return true;
            }
            /* character (ASCII or UTF-8 from IME): append to query.
               (input_mode guarantees ev_key is a character here) */
            {
                std::string ch = ev_key;
                if (ev_key == "space") ch = " ";
                std::string q = ((side == 0) ? cur.top_left_query
                                             : cur.top_right_query) + ch;
                if (side == 0) {
                    StateStore::instance().set_top_left_query(q);
                } else {
                    StateStore::instance().set_top_right_query(q);
                    restore_search_view(q);
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
        if (ev_key == "escape" && !cur.search_active && !cur.nav_stack.empty()) {
            StateStore::instance().nav_pop();
            return true;
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
                    int idx = cur.group_index;
                    if (idx >= (int)cur.groups.size() - 1) return true; /* at bottom */
                    if (idx == -1 && cur.groups.empty()) return true;
                    int next = (idx < 0) ? 0 : idx + 1;
                    StateStore::instance().set_group_index(next);
                    if (next >= 0 && next < (int)cur.groups.size()) {
                        std::vector<const char*> paths;
                        for (auto &s : cur.groups[next].songs) paths.push_back(s.id);
                    }
                } else {
                    int next = cur.netease_selected + 1;
                    if (next < (int)cur.netease_menu.size())
                        StateStore::instance().set_netease_selected(next);
                }
            } else {
                if (!cur.playlist.empty() && cur.selected_index < (int)cur.playlist.size() - 1)
                    StateStore::instance().set_selected_index(cur.selected_index + 1);
            }
            return true;

        case Action::MoveUp:
            if (cur.active_panel == 0) {
                if (cur.music_mode == MusicMode::Local) {
                    int prev = cur.group_index - 1;
                    if (prev < -1) return true; /* already at top (netease entry) */
                    if (prev >= 0) {
                        StateStore::instance().set_group_index(prev);
                        std::vector<const char*> paths;
                        for (auto &s : cur.groups[prev].songs) paths.push_back(s.id);
                    } else {
                        StateStore::instance().set_group_index(-1);
                    }
                } else {
                    int prev = cur.netease_selected - 1;
                    if (prev >= -1)
                        StateStore::instance().set_netease_selected(prev);
                }
            } else {
                if (cur.selected_index > 0)
                    StateStore::instance().set_selected_index(cur.selected_index - 1);
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
            /* Right panel: play selected song */
            if (cur.active_panel == 1)
                play_from_playlist(cur.selected_index);
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
            /* toggle the top search row (focused panel's box) — both modes */
            if (cur.top_search_active) {
                close_top_search();
            } else {
                StateStore::instance().set_top_search_active(true,
                                                              cur.active_panel);
            }
            return true;
        }

        case Action::ShowHelp:
            StateStore::instance().set_show_help(!cur.show_help);
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
           - re-place every ~0.5s: terminal resizes / fullscreen toggles /
             window switches can drop or relocate placed images, and a
             periodic re-place heals that with no fragile resize detection
           - clear when leaving lyric mode (edge-triggered) */
        bool gfx_active = false;
        int  gfx_tick = 0;
        while (!loop.HasQuitted()) {
            loop.RunOnceBlocking();
            if (term_gfx_active()) {
                const AppState &st = state.state();
                bool active = st.lyric_mode && st.cover.pixels &&
                              !st.show_help && st.login_state == 0;
                if (active) {
                    int cw = 0, dh = 0;
                    cover_layout(st, &cw, &dh);
                    if (cw > 0 && dh > 0) {
                        term_gfx_upload(&st.cover);
                        if (!gfx_active || ++gfx_tick >= 30) {
                            gfx_tick = 0;
                            /* place top-aligned below the 1-row top bar */
                            printf("\x1b[2;1H");
                            term_gfx_place(cw);
                        }
                    }
                    gfx_active = true;
                } else if (gfx_active) {
                    term_gfx_clear();
                    gfx_active = false;
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
