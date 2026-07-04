#pragma once
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Netease streaming pipe ────────────────────────────
 * Self-contained module.
 * playback_coordinator calls open/close, never touches
 * popen/curl/FIFO directly.                                       */

/* Open stream. Gets play URL from netease-cli, creates FIFO,
 * forks curl. Returns FIFO read-end FILE* and fills fifo_path.
 * Returns NULL on error (fifo_path untouched).                */
FILE *netease_stream_open(const char *song_id,
                          char *fifo_path_out, size_t path_size);

/* Kill curl child, remove FIFO, close FILE. Safe if f==NULL. */
void  netease_stream_close(FILE *f);

#ifdef __cplusplus
}
#endif
