/* http_range_server.c — see http_range_server.h. */
#include "http_range_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

struct HttpRangeServer {
    int listen_fd;
    int port;
    int stop;
    char path[4096];
    pthread_t thread;
    pthread_mutex_t lock;
    int requests;
};

/* Parse "Range: bytes=N-" / "bytes=N-M" (a missing end means EOF). */
static int parse_range(const char *req, int64_t size, int64_t *start, int64_t *end) {
    const char *p = strcasestr(req, "\r\nrange:");
    if (!p) return 0;
    p += 8;
    while (*p == ' ') p++;
    if (strncmp(p, "bytes=", 6) != 0) return 0;
    p += 6;
    if (*p == '-') return 0;   /* suffix range: not used by the client */
    *start = strtoll(p, (char **)&p, 10);
    *end = size - 1;
    if (*p == '-') {
        p++;
        if (*p >= '0' && *p <= '9') *end = strtoll(p, (char **)&p, 10);
    }
    return 1;
}

static void send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return;
        p += n;
        len -= (size_t)n;
    }
}

static void serve_one(HttpRangeServer *s, int fd) {
    /* Timeouts on BOTH directions: the loop is serial, so a client that
       connects without sending a request (FFmpeg opens speculative
       connections) or stops reading must not be able to block the accept
       loop forever — that hung the whole test suite. */
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    char req[8192];
    size_t used = 0;
    while (used + 1 < sizeof(req)) {
        ssize_t n = recv(fd, req + used, sizeof(req) - 1 - used, 0);
        if (n <= 0) return;
        used += (size_t)n;
        req[used] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (!strstr(req, "GET ") && !strstr(req, "HEAD ")) {
        send_all(fd, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n", 54);
        return;
    }

    FILE *f = fopen(s->path, "rb");
    if (!f) {
        send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 40);
        return;
    }
    fseek(f, 0, SEEK_END);
    int64_t size = ftell(f);

    int64_t start = 0, end = size - 1;
    int ranged = parse_range(req, size, &start, &end);
    if (ranged && (start >= size || start < 0 || end < start)) {
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 416 Range Not Satisfiable\r\n"
                         "Content-Range: bytes */%lld\r\nContent-Length: 0\r\n\r\n",
                         (long long)size);
        send_all(fd, hdr, (size_t)n);
        fclose(f);
        return;
    }
    if (end >= size) end = size - 1;
    int64_t len = end - start + 1;

    char hdr[512];
    int hn;
    if (ranged)
        hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Content-Range: bytes %lld-%lld/%lld\r\n"
                      "Content-Length: %lld\r\nConnection: close\r\n\r\n",
                      (long long)start, (long long)end, (long long)size,
                      (long long)len);
    else
        hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Content-Length: %lld\r\nConnection: close\r\n\r\n",
                      (long long)size);
    send_all(fd, hdr, (size_t)hn);

    int is_head = (strncmp(req, "HEAD ", 5) == 0);
    if (!is_head) {
        /* pread in chunks so a big file does not get fully copied per request */
        fseek(f, (long)start, SEEK_SET);
        char buf[64 * 1024];
        int64_t left = len;
        while (left > 0) {
            size_t want = left < (int64_t)sizeof(buf) ? (size_t)left : sizeof(buf);
            size_t rd = fread(buf, 1, want, f);
            if (rd == 0) break;
            send_all(fd, buf, rd);
            left -= (int64_t)rd;
        }
    }
    fclose(f);

    pthread_mutex_lock(&s->lock);
    s->requests++;
    pthread_mutex_unlock(&s->lock);
}

static void *server_main(void *arg) {
    HttpRangeServer *s = (HttpRangeServer *)arg;
    while (!s->stop) {
        /* poll() with a timeout so the loop notices `stop`: shutdown() on a
           listening socket does not wake a blocked accept() on Linux, which
           would make hrs_stop() hang in pthread_join(). */
        struct pollfd pfd = {s->listen_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) continue;
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (s->stop) break;
            continue;
        }
        serve_one(s, fd);
        close(fd);
    }
    return NULL;
}

HttpRangeServer *hrs_start(const char *file_path) {
    HttpRangeServer *s = (HttpRangeServer *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (snprintf(s->path, sizeof(s->path), "%s", file_path) >= (int)sizeof(s->path)) {
        free(s);
        return NULL;
    }
    pthread_mutex_init(&s->lock, NULL);

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) goto fail;
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;
    if (listen(s->listen_fd, 8) != 0) goto fail;
    socklen_t alen = sizeof(addr);
    if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen) != 0) goto fail;
    s->port = ntohs(addr.sin_port);
    if (pthread_create(&s->thread, NULL, server_main, s) != 0) goto fail;
    return s;

fail:
    if (s->listen_fd > 0) close(s->listen_fd);
    free(s);
    return NULL;
}

int hrs_port(const HttpRangeServer *s) { return s->port; }

void hrs_url(const HttpRangeServer *s, char *out, size_t sz) {
    snprintf(out, sz, "http://127.0.0.1:%d/media", s->port);
}

int hrs_request_count(const HttpRangeServer *s) {
    HttpRangeServer *m = (HttpRangeServer *)s;
    pthread_mutex_lock(&m->lock);
    int n = m->requests;
    pthread_mutex_unlock(&m->lock);
    return n;
}

void hrs_stop(HttpRangeServer *s) {
    if (!s) return;
    s->stop = 1;
    pthread_join(s->thread, NULL);
    close(s->listen_fd);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
