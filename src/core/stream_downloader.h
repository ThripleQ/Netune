#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

/* stream_downloader.h — background download thread that streams a URL into
 * a local .part file while playback reads it.
 *
 * The downloader is the single writer of the .part file; playback only
 * reads bytes below the current watermark. This decouples download from
 * playback: the download thread runs ahead (or lags behind) independently,
 * and playback blocks only when it reaches the watermark and the download
 * has not finished yet.
 *
 * The .part file is left on disk when the downloader is destroyed — the
 * caller decides whether to commit it (rename to a final cache entry),
 * keep it as a partial prefix, or remove it. */

typedef struct StreamDownloader StreamDownloader;

/* Create a downloader for url → part_path. part_path must be writable
   (its parent directory must exist). Returns NULL on failure. */
StreamDownloader *stream_downloader_create(const char *url, const char *part_path);

/* Create a resume/overwrite downloader that writes url at offset start_byte
   of part_path without truncating it (the file must already exist, e.g. a
   .part created by a sequential downloader, or an existing cache file being
   re-fetched). When start_byte > 0 an HTTP Range (bytes=start_byte-) is
   used, so it can fetch the resume region of a quality switch in parallel
   with the sequential download; start_byte == 0 re-fetches the whole file
   in place (used to backfill a holey cache without losing its retained
   tail). Returns NULL on failure. */
StreamDownloader *stream_downloader_create_resume(const char *url,
                                                  const char *part_path,
                                                  int64_t start_byte);

/* Start the download thread. 0 = ok, -1 = failure. */
int stream_downloader_start(StreamDownloader *dl);

/* Bytes written to the file so far (monotonic). */
int64_t stream_downloader_watermark(StreamDownloader *dl);

/* Total size (bytes) of the download from the HTTP Content-Length, or 0
   when unknown (chunked responses / headers not yet seen). The caller
   already waits for a prefix before opening the stream, so by the time
   playback starts the value is normally set. */
int64_t stream_downloader_total_size(const StreamDownloader *dl);

/* 1 = download finished cleanly (EOF reached), 0 = still downloading. */
int stream_downloader_done(StreamDownloader *dl);

/* 1 = download failed (network error, aborted), 0 = ok. */
int stream_downloader_failed(StreamDownloader *dl);

/* Block until watermark >= min_bytes, or the download finishes/fails, or
   timeout_ms elapses (timeout_ms < 0 = wait forever). When min_bytes < 0,
   waits for any watermark progress instead. Returns:
     1 = condition met (watermark reached or download done),
     0 = timeout,
    -1 = download failed. */
int stream_downloader_wait_watermark(StreamDownloader *dl, int64_t min_bytes,
                                     int timeout_ms);

/* Stop the download thread and free. The .part file is left on disk
   (caller decides commit/keep/remove). Safe to call while the download is
   in progress — the thread is asked to abort and is joined. */
void stream_downloader_destroy(StreamDownloader *dl);

#ifdef __cplusplus
}
#endif
