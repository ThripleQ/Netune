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

/* Open a stream that continues from a partial cache file: the cached
   prefix (the file's current size) is played from disk first, then the
   network source is resumed from that byte offset (HTTP Range) and the
   remaining bytes are appended back into the same file, so a full play
   backfills it into a complete cache entry. Seeking inside the cached
   prefix is served from disk (backfill stays contiguous); seeking past it
   switches to pure network and freezes the file (the partial prefix is
   kept). Returns NULL on failure. */
FFStream* ffstream_open_partial(const char *url, const char *prefix_path,
                                int *sample_rate, int *channels,
                                int *duration_sec);

/* Wait callback for ffstream_open_growing: invoked when the local file is
   exhausted at byte position `pos` but may still be growing (a background
   downloader is appending to it). The callback should block until data
   past `pos` is available, or the stream finishes/fails. Returns:
     1 = more data may be available (retry the read),
     0 = the stream is finished (EOF), -1 = error. */
typedef int (*ffstream_wait_fn)(void *opaque, int64_t pos);

/* Open a local file that may still be growing (e.g. a .part being written
   by a background downloader). Reads block via `wait` when the file is
   exhausted until more data arrives or the stream finishes, so FFmpeg
   never sees a spurious EOF mid-download. The file is read-only here; the
   downloader is the sole writer. Returns NULL on failure. */
FFStream* ffstream_open_growing(const char *path, ffstream_wait_fn wait,
                                void *wait_opaque,
                                int *sample_rate, int *channels,
                                int *duration_sec);

/* Decode up to max_frames of PCM S16. Returns actual frames decoded (0=EOF). */
int ffstream_decode(FFStream *s, int16_t *pcm, int max_frames);

/* Seek to timestamp in seconds. Returns 0 on success. */
int ffstream_seek(FFStream *s, int64_t timestamp_sec);

/* Whether the stream currently has an open recorder (.part being written). */
int ffstream_recording(const FFStream *s);

/* Finalize a recording: flush + rename the .part to final_path (for a
   partial continuation the file is already at final_path and is kept).
   0 = ok (the file is now final and survives close), -1 = no active
   recorder or rename failed (the .part is discarded). */
int ffstream_recorder_commit(FFStream *s, const char *final_path);

/* Media extension (".mp3", ".flac", ...) of the opened stream, derived
   from the probed container. */
const char *ffstream_media_ext(const FFStream *s);

/* 1 = the stream was opened with ffstream_open_growing (a local file still
   being appended by a background downloader). */
int ffstream_growing(const FFStream *s);

/* Declare the total byte size of a growing stream (from the download's
   Content-Length). Makes AVSEEK_SIZE report the true size and re-derives
   the format duration, so FFmpeg's duration estimate and seek byte math
   are correct while the file is still being written. total_bytes <= 0
   (or non-growing) is a no-op. */
void ffstream_set_growing_total(FFStream *s, int64_t total_bytes);

/* The declared total byte size of a growing stream, or 0 when unknown. */
int64_t ffstream_growing_total(const FFStream *s);

/* Declare the valid byte regions of a holey cache file being re-fetched in
   place: a contiguous prefix [0, prefix) and an optional tail [tail_at,
   file end). Positions inside the gap are served only once the background
   downloader covers them. Both <= 0 disables the holey mode. */
void ffstream_set_growing_regions(FFStream *s, int64_t prefix, int64_t tail_at);

/* Seconds of audio currently available in a growing stream (derived from
   the on-disk size and the caller-provided bitrate), or -1 when not
   growing / unknown. Seeks on a growing stream are clamped to this value
   by growing_seek, so the caller can clamp the UI seek target to it to
   keep progress in sync. */
int ffstream_growing_avail_sec(const FFStream *s, long bitrate);

/* Average bitrate of the opened stream (bits/sec), or 0 if unknown.
   For a growing file this is the probe-time estimate — the caller can
   combine it with the current on-disk size to track the real duration as
   the download progresses. */
long ffstream_bitrate(const FFStream *s);

/* Growing-file only: the byte position the decoder has consumed so far
   (read cursor of the local grow file). Lets the caller compute a measured
   average bitrate (bytes consumed / playtime) that avoids the unreliable
   probe-time estimate. Returns 0 when not growing / unknown. */
int64_t ffstream_growing_bytes_read(const FFStream *s);

/* Close and free. */
void ffstream_close(FFStream *s);

#ifdef __cplusplus
}
#endif
