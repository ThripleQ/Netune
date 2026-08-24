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

/* Open a stream and simultaneously tee the raw stream bytes into
   `rec_path` (a cache .part file). The recorder starts after probe
   completes (i.e. at the first decoded byte), so the .part always begins
   at the stream's first byte. Any seek (ffstream_seek) or close before
   ffstream_recorder_commit discards the .part file. Returns NULL on
   failure. `rec_path` may be NULL to skip recording. */
FFStream* ffstream_open_rec(const char *url, const char *rec_path,
                            int *sample_rate, int *channels,
                            int *duration_sec);

/* Decode up to max_frames of PCM S16. Returns actual frames decoded (0=EOF). */
int ffstream_decode(FFStream *s, int16_t *pcm, int max_frames);

/* Seek to timestamp in seconds. Returns 0 on success. */
int ffstream_seek(FFStream *s, int64_t timestamp_sec);

/* Whether the stream currently has an open recorder (.part being written). */
int ffstream_recording(const FFStream *s);

/* Finalize a complete recording: flush + rename the .part to final_path.
   0 = ok (the file is now final and survives close), -1 = no active
   recorder or rename failed (the .part is discarded). */
int ffstream_recorder_commit(FFStream *s, const char *final_path);

/* Media extension (".mp3", ".flac", ...) of the opened stream, derived
   from the probed container. */
const char *ffstream_media_ext(const FFStream *s);

/* Close and free. */
void ffstream_close(FFStream *s);

#ifdef __cplusplus
}
#endif
