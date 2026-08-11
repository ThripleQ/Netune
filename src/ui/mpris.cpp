#include "ui/mpris.h"

#ifndef _WIN32

#include <dbus/dbus.h>
#include <pthread.h>
#include <string.h>

#include <string>

#include "core/event_bus.h"
#include "infra/log.h"
#include "ui/state_store.h"

#define MPRIS_SERVICE  "org.mpris.MediaPlayer2.netune"
#define MPRIS_OBJECT   "/org/mpris/MediaPlayer2"
#define MPRIS_IFACE    "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER   "org.mpris.MediaPlayer2.Player"

static DBusConnection *g_conn = NULL;
static pthread_t       g_thread;
static volatile int    g_running = 0;

/* ── Shared snapshot (written by main thread via mpris_sync,
      read by the D-Bus thread under the same mutex) ── */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::string g_playback_status = "Stopped";  /* Playing/Paused/Stopped */
static std::string g_track_id;                     /* mpris:trackid suffix */
static std::string g_title, g_artist, g_album, g_art_url;
static int64_t g_length_us = 0;
static int64_t g_position_us = 0;
static int      g_volume_pct = 80;                 /* 0..100 */
static bool     g_muted = false;
static int      g_loop_mode = 0;                   /* 0=None 1=Track 2=Playlist 3=Shuffle */

/* ── D-Bus helpers ─────────────────────────────────── */

static void append_variant(DBusMessageIter *dict, const char *name,
                           int type, const void *val) {
    DBusMessageIter entry, variant;
    char sig[2] = {(char)type, '\0'};
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant);
    dbus_message_iter_append_basic(&variant, type, val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

static void append_variant_bool(DBusMessageIter *dict, const char *name,
                                dbus_bool_t val) {
    append_variant(dict, name, DBUS_TYPE_BOOLEAN, &val);
}

static void append_variant_int64(DBusMessageIter *dict, const char *name,
                                 int64_t val) {
    dbus_int64_t v = val;
    append_variant(dict, name, DBUS_TYPE_INT64, &v);
}

static void append_variant_double(DBusMessageIter *dict, const char *name,
                                  double val) {
    append_variant(dict, name, DBUS_TYPE_DOUBLE, &val);
}

static void append_variant_string(DBusMessageIter *dict, const char *name,
                                  const std::string &val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_STRING_AS_STRING, &variant);
    const char *s = val.c_str();
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &s);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

/* mpris:artist is an array of strings (as) */
static void append_variant_artist(DBusMessageIter *dict, const std::string &artist) {
    DBusMessageIter entry, variant, arr;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *name = "xesam:artist";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_ARRAY_AS_STRING
                                     DBUS_TYPE_STRING_AS_STRING, &variant);
    dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_STRING_AS_STRING, &arr);
    if (!artist.empty()) {
        const char *s = artist.c_str();
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &s);
    }
    dbus_message_iter_close_container(&variant, &arr);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

/* ── Build Metadata a{sv} ──────────────────────────── */
static void fill_metadata(DBusMessageIter *dict) {
    pthread_mutex_lock(&g_mutex);

    if (!g_track_id.empty()) {
        std::string trackid = "/netune/" + g_track_id;
        append_variant_string(dict, "mpris:trackid", trackid);
    }
    if (!g_title.empty())
        append_variant_string(dict, "xesam:title", g_title);
    if (!g_artist.empty())
        append_variant_artist(dict, g_artist);
    if (!g_album.empty())
        append_variant_string(dict, "xesam:album", g_album);
    if (!g_art_url.empty())
        append_variant_string(dict, "mpris:artUrl", g_art_url);
    if (g_length_us > 0)
        append_variant_int64(dict, "mpris:length", g_length_us);

    pthread_mutex_unlock(&g_mutex);
}

/* ── Append an empty string-array property (as) ────── */
static void append_variant_empty_string_array(DBusMessageIter *dict,
                                              const char *name) {
    DBusMessageIter e, v, arr;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT,
        DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING, &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY,
        DBUS_TYPE_STRING_AS_STRING, &arr);
    dbus_message_iter_close_container(&v, &arr);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

/* ── GetAll ────────────────────────────────────────── */
static void handle_get_all(DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return;

    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
        DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
        DBUS_TYPE_STRING_AS_STRING
        DBUS_TYPE_VARIANT_AS_STRING
        DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

    /* org.mpris.MediaPlayer2 */
    append_variant_string(&dict, "Identity", "Netune");
    append_variant_string(&dict, "DesktopEntry", "netune");
    append_variant_bool(&dict, "CanQuit", FALSE);
    append_variant_bool(&dict, "CanRaise", FALSE);
    append_variant_bool(&dict, "HasTrackList", FALSE);
    append_variant_empty_string_array(&dict, "SupportedUriSchemes");
    append_variant_empty_string_array(&dict, "SupportedMimeTypes");

    /* org.mpris.MediaPlayer2.Player */
    pthread_mutex_lock(&g_mutex);
    append_variant_string(&dict, "PlaybackStatus", g_playback_status);
    const char *loop = (g_loop_mode == 1) ? "Track" :
                       (g_loop_mode == 2) ? "Playlist" : "None";
    append_variant_string(&dict, "LoopStatus", loop);
    pthread_mutex_unlock(&g_mutex);

    dbus_bool_t btrue = TRUE;
    double rate = 1.0;
    pthread_mutex_lock(&g_mutex);
    dbus_bool_t shuffle = (g_loop_mode == 3) ? TRUE : FALSE;
    pthread_mutex_unlock(&g_mutex);
    append_variant_bool(&dict, "Shuffle", shuffle);
    append_variant_double(&dict, "Rate", rate);
    fill_metadata(&dict);
    pthread_mutex_lock(&g_mutex);
    double volume = g_muted ? 0.0 : (double)g_volume_pct / 100.0;
    pthread_mutex_unlock(&g_mutex);
    append_variant_double(&dict, "Volume", volume);
    pthread_mutex_lock(&g_mutex);
    append_variant_int64(&dict, "Position", g_position_us);
    pthread_mutex_unlock(&g_mutex);
    append_variant_double(&dict, "MinimumRate", rate);
    append_variant_double(&dict, "MaximumRate", rate);
    append_variant_bool(&dict, "CanGoNext", btrue);
    append_variant_bool(&dict, "CanGoPrevious", btrue);
    append_variant_bool(&dict, "CanPlay", btrue);
    append_variant_bool(&dict, "CanPause", btrue);
    append_variant_bool(&dict, "CanSeek", btrue);
    append_variant_bool(&dict, "CanControl", btrue);

    dbus_message_iter_close_container(&iter, &dict);
    dbus_connection_send(g_conn, reply, NULL);
    dbus_message_unref(reply);
}

/* ── Get ───────────────────────────────────────────── */
static void handle_get(DBusMessage *msg) {
    DBusMessageIter in;
    if (!dbus_message_iter_init(msg, &in)) return;
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRING) return;
    const char *iface_skip = NULL;
    dbus_message_iter_get_basic(&in, &iface_skip);
    (void)iface_skip;  /* interface name — we serve all interfaces at this path */
    if (!dbus_message_iter_next(&in)) return;
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRING) return;
    const char *prop = NULL;
    dbus_message_iter_get_basic(&in, &prop);
    if (!prop) return;

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    DBusMessageIter out, variant;
    dbus_message_iter_init_append(reply, &out);

    /* Read snapshot under lock, copy into locals */
    std::string status, loop, art_url;
    int64_t position_us = 0;
    int volume_pct = 0;
    bool muted = false, found = false;
    pthread_mutex_lock(&g_mutex);
    status     = g_playback_status;
    loop       = (g_loop_mode == 1) ? "Track" :
                 (g_loop_mode == 2) ? "Playlist" : "None";
    art_url    = g_art_url;
    position_us = g_position_us;
    volume_pct = g_volume_pct;
    muted      = g_muted;
    pthread_mutex_unlock(&g_mutex);

    if (strcmp(prop, "PlaybackStatus") == 0) {
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_STRING_AS_STRING, &variant);
        const char *v = status.c_str();
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
        found = true;
    } else if (strcmp(prop, "LoopStatus") == 0) {
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_STRING_AS_STRING, &variant);
        const char *v = loop.c_str();
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
        found = true;
    } else if (strcmp(prop, "Shuffle") == 0) {
        dbus_bool_t v = (g_loop_mode == 3) ? TRUE : FALSE;
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &v);
        found = true;
    } else if (strcmp(prop, "Identity") == 0) {
        const char *v = "Netune";
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_STRING_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
        found = true;
    } else if (strcmp(prop, "DesktopEntry") == 0) {
        const char *v = "netune";
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_STRING_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
        found = true;
    } else if (strcmp(prop, "Volume") == 0) {
        double v = muted ? 0.0 : (double)volume_pct / 100.0;
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_DOUBLE_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_DOUBLE, &v);
        found = true;
    } else if (strcmp(prop, "Position") == 0) {
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_INT64_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT64,
                                       &position_us);
        found = true;
    } else if (strcmp(prop, "CanQuit") == 0 || strcmp(prop, "CanRaise") == 0 ||
               strcmp(prop, "HasTrackList") == 0) {
        dbus_bool_t b = FALSE;
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &b);
        found = true;
    }

    if (found) {
        dbus_message_iter_close_container(&out, &variant);
        dbus_connection_send(g_conn, reply, NULL);
    } else {
        DBusMessage *err = dbus_message_new_error(
            msg, "org.freedesktop.DBus.Error.InvalidArgs",
            "No such property");
        if (err) { dbus_connection_send(g_conn, err, NULL); dbus_message_unref(err); }
    }
    dbus_message_unref(reply);
}

/* ── Method handlers ───────────────────────────────── */

static void mpris_reply_empty(DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply) {
        dbus_connection_send(g_conn, reply, NULL);
        dbus_message_unref(reply);
    }
}

/* Play/Pause are sent as dedicated playback events (no command payload);
   PlayPause/Stop/Next/Prev go through EV_MPRIS_COMMAND. */
static void handle_pause(DBusMessage *msg) {
    event_bus_publish(EV_PLAYBACK_PAUSE, NULL, 0);
    mpris_reply_empty(msg);
}

static void handle_play(DBusMessage *msg) {
    event_bus_publish(EV_PLAYBACK_RESUME, NULL, 0);
    mpris_reply_empty(msg);
}

/* publish an int-payload MPRIS command to the app (main thread) */
static void mpris_publish_cmd(int cmd, int payload) {
    int data[2] = { cmd, payload };
    event_bus_publish(EV_MPRIS_COMMAND, data, sizeof(data));
}

/* send a command with no payload and reply */
static void mpris_cmd_and_reply(int cmd, DBusMessage *msg) {
    mpris_publish_cmd(cmd, 0);
    mpris_reply_empty(msg);
}

static void handle_play_pause(DBusMessage *msg) {
    mpris_cmd_and_reply(MPRIS_CMD_PLAYPAUSE, msg);
}

static void handle_stop(DBusMessage *msg) {
    mpris_cmd_and_reply(MPRIS_CMD_STOP, msg);
}

static void handle_next(DBusMessage *msg) {
    mpris_cmd_and_reply(MPRIS_CMD_NEXT, msg);
}

static void handle_previous(DBusMessage *msg) {
    mpris_cmd_and_reply(MPRIS_CMD_PREV, msg);
}

/* Seek(Offset x): relative offset in microseconds → absolute target sec */
static void handle_seek(DBusMessage *msg) {
    dbus_int64_t offset_us = 0;
    DBusMessageIter in;
    if (dbus_message_iter_init(msg, &in) &&
        dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_INT64) {
        dbus_message_iter_get_basic(&in, &offset_us);
    }
    pthread_mutex_lock(&g_mutex);
    int target_sec = (int)((g_position_us + offset_us) / 1000000);
    pthread_mutex_unlock(&g_mutex);
    if (target_sec < 0) target_sec = 0;
    mpris_publish_cmd(MPRIS_CMD_SEEK, target_sec);
    mpris_reply_empty(msg);
}

/* SetPosition(TrackId o, Position x): absolute position in µs */
static void handle_set_position(DBusMessage *msg) {
    DBusMessageIter in;
    dbus_int64_t pos_us = 0;
    if (dbus_message_iter_init(msg, &in)) {
        if (dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_OBJECT_PATH) {
            /* skip TrackId — only one track at a time */
            dbus_message_iter_next(&in);
        }
        if (dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_INT64)
            dbus_message_iter_get_basic(&in, &pos_us);
    }
    int target_sec = (int)(pos_us / 1000000);
    if (target_sec < 0) target_sec = 0;
    mpris_publish_cmd(MPRIS_CMD_SEEK, target_sec);
    mpris_reply_empty(msg);
}

/* ── Introspect XML ────────────────────────────────── */
static const char *MPRIS_INTROSPECT_XML =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.mpris.MediaPlayer2\">\n"
    "    <method name=\"Raise\"/>\n"
    "    <method name=\"Quit\"/>\n"
    "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"DesktopEntry\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
    "    <method name=\"Play\"/>\n"
    "    <method name=\"Pause\"/>\n"
    "    <method name=\"PlayPause\"/>\n"
    "    <method name=\"Stop\"/>\n"
    "    <method name=\"Next\"/>\n"
    "    <method name=\"Previous\"/>\n"
    "    <method name=\"Seek\">\n"
    "      <arg name=\"Offset\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"SetPosition\">\n"
    "      <arg name=\"TrackId\" type=\"o\" direction=\"in\"/>\n"
    "      <arg name=\"Position\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"OpenUri\">\n"
    "      <arg name=\"Uri\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"LoopStatus\" type=\"s\" access=\"readwrite\"/>\n"
    "    <property name=\"Rate\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Shuffle\" type=\"b\" access=\"readwrite\"/>\n"
    "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
    "    <property name=\"Volume\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
    "    <property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"MaximumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "</node>\n";

static void handle_introspect(DBusMessage *msg) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply) {
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
                                       &MPRIS_INTROSPECT_XML);
        dbus_connection_send(g_conn, reply, NULL);
        dbus_message_unref(reply);
    }
}

/* ── PropertiesChanged signal ──────────────────────── */
/* Emit PropertiesChanged(iface, {prop: value}, {}) for a single basic-typed
   property. `value` points to a value of the given dbus type. */
static void emit_properties_changed(const char *iface_name,
                                    const std::string &prop,
                                    int type, const void *value) {
    if (!g_conn || !g_running) return;
    DBusMessage *sig = dbus_message_new_signal(
        MPRIS_OBJECT, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    if (!sig) return;

    DBusMessageIter iter, arr, entry, variant;
    dbus_message_iter_init_append(sig, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface_name);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
        DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
        DBUS_TYPE_STRING_AS_STRING
        DBUS_TYPE_VARIANT_AS_STRING
        DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &arr);
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *name = prop.c_str();
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    char sig_str[2] = {(char)type, '\0'};
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig_str, &variant);
    dbus_message_iter_append_basic(&variant, type, value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&arr, &entry);
    dbus_message_iter_close_container(&iter, &arr);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
        DBUS_TYPE_STRING_AS_STRING, &arr);
    dbus_message_iter_close_container(&iter, &arr);

    dbus_connection_send(g_conn, sig, NULL);
    dbus_message_unref(sig);
}

static void emit_properties_changed_string(const char *iface_name,
                                           const std::string &prop,
                                           const std::string &val) {
    if (!g_conn || !g_running) return;
    DBusMessage *sig = dbus_message_new_signal(
        MPRIS_OBJECT, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    if (!sig) return;

    DBusMessageIter iter, arr, entry, variant;
    dbus_message_iter_init_append(sig, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface_name);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
        DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
        DBUS_TYPE_STRING_AS_STRING
        DBUS_TYPE_VARIANT_AS_STRING
        DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &arr);
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char *name = prop.c_str();
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_STRING_AS_STRING, &variant);
    const char *v = val.c_str();
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&arr, &entry);
    dbus_message_iter_close_container(&iter, &arr);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
        DBUS_TYPE_STRING_AS_STRING, &arr);
    dbus_message_iter_close_container(&iter, &arr);

    dbus_connection_send(g_conn, sig, NULL);
    dbus_message_unref(sig);
}

/* Emit PropertiesChanged for a full Metadata replacement */
static void emit_metadata_changed(void) {
    if (!g_conn || !g_running) return;
    DBusMessage *sig = dbus_message_new_signal(
        MPRIS_OBJECT, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    if (!sig) return;

    DBusMessageIter iter, arr, e, v, md;
    dbus_message_iter_init_append(sig, &iter);
    const char *ifn = MPRIS_PLAYER;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &ifn);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
        DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
        DBUS_TYPE_STRING_AS_STRING
        DBUS_TYPE_VARIANT_AS_STRING
        DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &arr);

    const char *name = "Metadata";
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT,
        DBUS_TYPE_ARRAY_AS_STRING
        DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
        DBUS_TYPE_STRING_AS_STRING
        DBUS_TYPE_VARIANT_AS_STRING
        DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY,
        DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
        DBUS_TYPE_STRING_AS_STRING
        DBUS_TYPE_VARIANT_AS_STRING
        DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &md);
    fill_metadata(&md);
    dbus_message_iter_close_container(&v, &md);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(&arr, &e);
    dbus_message_iter_close_container(&iter, &arr);

    /* no invalidated props */
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
        DBUS_TYPE_STRING_AS_STRING, &arr);
    dbus_message_iter_close_container(&iter, &arr);

    dbus_connection_send(g_conn, sig, NULL);
    dbus_message_unref(sig);
}

/* ── Properties.Set ────────────────────────────────── */
/* Handles writable properties: LoopStatus, Shuffle, Volume. */
static void handle_properties_set(DBusMessage *msg) {
    DBusMessageIter in;
    if (!dbus_message_iter_init(msg, &in)) return;

    /* interface name — empty string is the MPRIS convention for
       "the property name is unique", accept it as-is */
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRING) return;
    const char *iface_name = NULL;
    dbus_message_iter_get_basic(&in, &iface_name);
    if (iface_name && iface_name[0] &&
        strcmp(iface_name, MPRIS_PLAYER) != 0) {
        mpris_reply_empty(msg);  /* ignore other interfaces */
        return;
    }
    if (!dbus_message_iter_next(&in)) return;

    /* property name */
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRING) return;
    const char *prop = NULL;
    dbus_message_iter_get_basic(&in, &prop);
    if (!prop) return;
    if (!dbus_message_iter_next(&in)) return;

    /* variant value */
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_VARIANT) return;
    DBusMessageIter v;
    dbus_message_iter_recurse(&in, &v);

    if (strcmp(prop, "LoopStatus") == 0 &&
        dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) {
        const char *s = NULL;
        dbus_message_iter_get_basic(&v, &s);
        int mode = (s && strcmp(s, "Track") == 0) ? 1 :
                   (s && strcmp(s, "Playlist") == 0) ? 2 : 0;
        event_bus_publish(EV_PLAYLIST_CHANGED, &mode, sizeof(mode));
    } else if (strcmp(prop, "Shuffle") == 0 &&
               dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t on = FALSE;
        dbus_message_iter_get_basic(&v, &on);
        /* 3 = Shuffle, 0 = None (shuffle off keeps current queue order) */
        int mode = on ? 3 : 0;
        event_bus_publish(EV_PLAYLIST_CHANGED, &mode, sizeof(mode));
    } else if (strcmp(prop, "Volume") == 0 &&
               dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_DOUBLE) {
        double d = 1.0;
        dbus_message_iter_get_basic(&v, &d);
        if (d < 0.0) d = 0.0;
        if (d > 1.0) d = 1.0;
        int vol = (int)(d * 100.0 + 0.5);
        event_bus_publish(EV_VOLUME_CHANGED, &vol, sizeof(vol));
    }

    mpris_reply_empty(msg);
}

/* ── Main message handler ──────────────────────────── */
static DBusHandlerResult mpris_message_handler(DBusConnection *conn,
                                               DBusMessage *msg,
                                               void *user_data) {
    (void)conn; (void)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *iface  = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0 &&
        strcmp(member, "Introspect") == 0) {
        handle_introspect(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
        if (strcmp(member, "GetAll") == 0) {
            handle_get_all(msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (strcmp(member, "Get") == 0) {
            handle_get(msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (strcmp(member, "Set") == 0) {
            handle_properties_set(msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (strcmp(iface, MPRIS_PLAYER) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(member, "Pause") == 0) {
        handle_pause(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "Play") == 0) {
        handle_play(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "PlayPause") == 0) {
        handle_play_pause(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "Stop") == 0) {
        handle_stop(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "Next") == 0) {
        handle_next(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "Previous") == 0) {
        handle_previous(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "Seek") == 0) {
        handle_seek(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "SetPosition") == 0) {
        handle_set_position(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable g_vtable = {
    .message_function = mpris_message_handler,
};

/* ── D-Bus dispatch thread ─────────────────────────── */

static void *mpris_thread_main(void *arg) {
    (void)arg;
    while (g_running) {
        if (!dbus_connection_read_write_dispatch(g_conn, 200))
            break;  /* connection closed */
    }
    return NULL;
}

/* ── Public API ────────────────────────────────────── */

int mpris_init(void) {
    DBusError err;
    dbus_error_init(&err);

    /* libdbus: enable thread support (send() from main thread) */
    dbus_threads_init_default();

    g_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!g_conn) {
        LOG_WARN("MPRIS: session bus unavailable: %s",
                 err.message ? err.message : "unknown");
        dbus_error_free(&err);
        return -1;
    }

    int ret = dbus_bus_request_name(g_conn, MPRIS_SERVICE,
                                    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        LOG_WARN("MPRIS: failed to acquire name %s: %s",
                 MPRIS_SERVICE, err.message ? err.message : "unknown");
        dbus_error_free(&err);
        dbus_connection_unref(g_conn);
        g_conn = NULL;
        return -1;
    }

    if (!dbus_connection_try_register_object_path(g_conn, MPRIS_OBJECT,
                                                  &g_vtable, NULL, &err)) {
        LOG_WARN("MPRIS: failed to register object path: %s",
                 err.message ? err.message : "unknown");
        dbus_error_free(&err);
        dbus_connection_unref(g_conn);
        g_conn = NULL;
        return -1;
    }

    g_running = 1;
    if (pthread_create(&g_thread, NULL, mpris_thread_main, NULL) != 0) {
        g_running = 0;
        dbus_connection_unref(g_conn);
        g_conn = NULL;
        return -1;
    }

    LOG_INFO("MPRIS: registered %s", MPRIS_SERVICE);
    return 0;
}

void mpris_shutdown(void) {
    if (!g_conn) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
    dbus_connection_close(g_conn);
    dbus_connection_unref(g_conn);
    g_conn = NULL;
    LOG_INFO("MPRIS: shut down");
}

/* ── State sync (main thread, once per frame) ──────── */
void mpris_sync(const void *state_ptr) {
    if (!g_conn || !state_ptr) return;
    const AppState &st = *(const AppState*)state_ptr;

    const char *status = "Stopped";
    if (st.playback_state == PlaybackState::Playing) status = "Playing";
    else if (st.playback_state == PlaybackState::Paused) status = "Paused";

    std::string track_id = st.current_song.id ? st.current_song.id : "";
    std::string title    = st.current_song.title ? st.current_song.title : "";
    std::string artist   = st.current_song.artist ? st.current_song.artist : "";
    std::string album    = st.current_song.album ? st.current_song.album : "";
    std::string art_url  = st.current_song.cover_url ? st.current_song.cover_url : "";
    int64_t length_us = (int64_t)st.total_time_sec * 1000000;
    int64_t pos_us    = (int64_t)st.current_time_sec * 1000000;

    /* Detect changes while holding the lock */
    bool status_changed = false, track_changed = false, volume_changed = false,
         loop_changed = false;
    pthread_mutex_lock(&g_mutex);
    status_changed  = (g_playback_status != status);
    track_changed   = (g_track_id != track_id || g_title != title ||
                       g_artist != artist || g_album != album ||
                       g_art_url != art_url || g_length_us != length_us);
    volume_changed  = (g_volume_pct != st.volume || g_muted != st.muted);
    loop_changed    = (g_loop_mode != (int)st.loop_mode);
    g_playback_status = status;
    g_track_id   = track_id;
    g_title      = title;
    g_artist     = artist;
    g_album      = album;
    g_art_url    = art_url;
    g_length_us  = length_us;
    g_position_us = pos_us;
    g_volume_pct = st.volume;
    g_muted      = st.muted;
    g_loop_mode  = (int)st.loop_mode;
    pthread_mutex_unlock(&g_mutex);

    if (status_changed)
        emit_properties_changed_string(MPRIS_PLAYER, "PlaybackStatus", status);
    if (track_changed)
        emit_metadata_changed();
    if (volume_changed) {
        pthread_mutex_lock(&g_mutex);
        double volume = g_muted ? 0.0 : (double)g_volume_pct / 100.0;
        pthread_mutex_unlock(&g_mutex);
        emit_properties_changed(MPRIS_PLAYER, "Volume", DBUS_TYPE_DOUBLE, &volume);
    }
    if (loop_changed) {
        pthread_mutex_lock(&g_mutex);
        const char *loop = (g_loop_mode == 1) ? "Track" :
                           (g_loop_mode == 2) ? "Playlist" : "None";
        dbus_bool_t shuffle = (g_loop_mode == 3) ? TRUE : FALSE;
        pthread_mutex_unlock(&g_mutex);
        emit_properties_changed_string(MPRIS_PLAYER, "LoopStatus", loop);
        emit_properties_changed(MPRIS_PLAYER, "Shuffle", DBUS_TYPE_BOOLEAN, &shuffle);
    }
}

#endif /* !_WIN32 */
