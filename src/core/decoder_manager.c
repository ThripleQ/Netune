#include "decoder_manager.h"
#include "decoder.h"
#include "infra/log.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Plugin registry ───────────────────────────────── */
#define MAX_DECODER_PLUGINS 8

static DecoderPlugin *g_plugins[MAX_DECODER_PLUGINS];
static int             g_plugin_count = 0;

int decoder_register_plugin(DecoderPlugin *plugin) {
    if (!plugin || !plugin->name || !plugin->ext) return -1;
    if (g_plugin_count >= MAX_DECODER_PLUGINS) return -1;
    g_plugins[g_plugin_count++] = plugin;
    LOG_INFO("Decoder plugin registered: %s (.%s)", plugin->name, plugin->ext);
    return 0;
}

/* Built-in plugins — declare extern */
extern DecoderPlugin g_mp3_plugin;
extern DecoderPlugin g_flac_plugin;
extern DecoderPlugin g_wav_plugin;

static void register_builtins(void) {
    static int done = 0;
    if (done) return;
    decoder_register_plugin(&g_mp3_plugin);
    decoder_register_plugin(&g_flac_plugin);
    decoder_register_plugin(&g_wav_plugin);
    done = 1;
}

/* ── Decoder (opaque) ──────────────────────────────── */
struct Decoder {
    DecoderPlugin *plugin;
    void          *handle;
    DecoderInfo    info;
};

bool decoder_supports_ext(const char *ext) {
    if (!ext) return false;
    register_builtins();
    for (int i = 0; i < g_plugin_count; i++) {
        if (strcasecmp(ext, g_plugins[i]->ext) == 0) return true;
    }
    return false;
}

Decoder* decoder_open(const char *path) {
    if (!path) return NULL;
    register_builtins();

    /* detect extension */
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    const char *ext = dot + 1;

    DecoderPlugin *plugin = NULL;
    for (int i = 0; i < g_plugin_count; i++) {
        if (strcasecmp(ext, g_plugins[i]->ext) == 0) {
            plugin = g_plugins[i];
            break;
        }
    }
    if (!plugin) {
        LOG_ERROR("No decoder for .%s", ext);
        return NULL;
    }

    void *handle = plugin->open(path);
    if (!handle) {
        LOG_ERROR("Plugin %s failed to open: %s", plugin->name, path);
        return NULL;
    }

    Decoder *d = (Decoder*)calloc(1, sizeof(Decoder));
    d->plugin = plugin;
    d->handle = handle;
    plugin->get_info(handle, &d->info.sample_rate,
                     &d->info.channels,
                     &d->info.total_frames);

    LOG_INFO("Decoded opened: %s  (%dHz %dch %dframes) via %s",
             path, d->info.sample_rate, d->info.channels,
             d->info.total_frames, plugin->name);
    return d;
}

int decoder_get_info(const Decoder *d, DecoderInfo *info) {
    if (!d || !info) return -1;
    *info = d->info;
    return 0;
}

int decoder_decode(Decoder *d, int16_t *pcm, int max_frames) {
    if (!d || !pcm || max_frames <= 0) return -1;

    DecodedFrame frame = {0};
    d->plugin->decode(d->handle, &frame);

    if (frame.frames <= 0) return frame.frames;

    /* copy from DecodedFrame to caller's buffer */
    int copy_frames = frame.frames < max_frames ? frame.frames : max_frames;
    memcpy(pcm, frame.data,
           (size_t)copy_frames * d->info.channels * sizeof(int16_t));
    free(frame.data);
    return copy_frames;
}

int decoder_seek(Decoder *d, int frame) {
    if (!d) return -1;
    return d->plugin->seek(d->handle, frame);
}

void decoder_close(Decoder *d) {
    if (!d) return;
    d->plugin->close(d->handle);
    free(d);
}
