#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

/* FFmpeg-based stream decoder. Takes a URL, decodes to PCM S16.
   Handles HTTP streaming, buffering, reconnection internally. */

typedef struct FFStream FFStream;

/* Open stream. Returns NULL on failure. */
FFStream* ffstream_open(const char *url,
                        int *sample_rate, int *channels, int *duration_sec);

/* Decode up to max_frames of PCM S16. Returns actual frames decoded (0=EOF). */
int ffstream_decode(FFStream *s, int16_t *pcm, int max_frames);

/* Seek to timestamp in seconds. Returns 0 on success. */
int ffstream_seek(FFStream *s, int64_t timestamp_sec);

/* Close and free. */
void ffstream_close(FFStream *s);

#ifdef __cplusplus
}
#endif
