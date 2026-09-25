/* http_range_server.h — a minimal local HTTP/1.1 server with Range support.
 *
 * The streaming tests need a real HTTP source that behaves like a CDN:
 * full GETs plus "Range: bytes=N-" resume requests (FFmpeg's http protocol
 * sends them for ffstream_open_partial / the resume downloader). Implemented
 * on plain POSIX sockets so the suite has no server dependency. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>

typedef struct HttpRangeServer HttpRangeServer;

/* Serve `file_path` on 127.0.0.1:<ephemeral>. NULL on failure. */
HttpRangeServer *hrs_start(const char *file_path);

int hrs_port(const HttpRangeServer *s);

/* Writes "http://127.0.0.1:<port>/media" into out. */
void hrs_url(const HttpRangeServer *s, char *out, size_t sz);

/* Number of requests served so far (Range ones included). */
int hrs_request_count(const HttpRangeServer *s);

void hrs_stop(HttpRangeServer *s);

#ifdef __cplusplus
}
#endif
