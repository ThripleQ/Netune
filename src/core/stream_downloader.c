#include "core/stream_downloader.h"
#include "infra/log.h"
#include "compat/utf8.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>   /* GetTickCount64 + DeviceIoControl */
#include <winioctl.h>  /* FSCTL_SET_SPARSE */
#include <io.h>        /* _fileno/_get_osfhandle for the sparse flag */
#endif

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#ifdef _WIN32
/* Mark the just-opened .part file as sparse. A downloader that restarts
   at a later byte offset (seek mode) fseeks past a hole and writes at the
   far end; on NTFS a non-sparse file materializes the whole gap as real
   zero blocks, so the "cache size" accounting (file size) inflates even
   though no audio ever touched the gap. FSCTL_SET_SPARSE makes holes cost
   nothing until actually written. */
static void mark_part_sparse(FILE *fp) {
    int fd = _fileno(fp);
    if (fd < 0) return;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD dummy = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dummy, NULL);
}
#endif

struct StreamDownloader {
    char        *url;
    char        *part_path;
    pthread_t    thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    FILE        *fp;          /* write handle, owned by the download thread */
    int64_t      watermark;   /* bytes written so far (relative to start_byte) */
    int64_t      total;       /* total size from Content-Length (0 = unknown) */
    int64_t      start_byte;  /* byte offset the file is written at (>= 0) */
    int          no_truncate; /* open "r+b" and keep existing file content */
    int          range_denied;/* resume: server ignored Range, returned 200 */
    int          started;
    int          done;        /* download reached EOF cleanly */
    int          failed;      /* network error / aborted */
    int          stop;        /* request the download thread to abort */
    int          write_errno; /* errno of the last short/failed fwrite (0=none) */
};

static int64_t now_mono_ms(void) {
#ifdef _WIN32
    /* The MSVC pthread shim (compat/msvc/pthread.h) has no clock_gettime /
       CLOCK_MONOTONIC; GetTickCount64 is the portable monotonic clock. */
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

#ifdef HAVE_LIBCURL
/* Write callback: append bytes to the .part file and advance the watermark.
   Runs on the download thread. */
static size_t dl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    StreamDownloader *dl = (StreamDownloader*)ud;
    size_t n = size * nmemb;
    /* The server ignored the Range request and answered 200 (full body):
       abort before writing the wrong bytes at the resume offset — the
       caller then falls back to the sequential downloader. */
    if (dl->start_byte > 0 && dl->range_denied) return 0;
    size_t w = fwrite(ptr, 1, n, dl->fp);
    pthread_mutex_lock(&dl->mutex);
    dl->watermark += (int64_t)w;
    if (w < n) dl->write_errno = errno;   /* e.g. ENOSPC on a full disk */
    pthread_cond_broadcast(&dl->cond);
    pthread_mutex_unlock(&dl->mutex);
    return w;
}

/* Response-status line parser for the resume (Range) downloader: if the
   server ignores the Range header and answers 200, flag range_denied so
   the write callback aborts. A later 206 (redirect chain) clears it. Runs
   on the download thread, before any body bytes. */
static size_t dl_range_status_cb(char *buffer, size_t size, size_t nitems,
                                 void *ud) {
    StreamDownloader *dl = (StreamDownloader*)ud;
    size_t n = size * nitems;
    if (n > 5 && strncmp(buffer, "HTTP/", 5) == 0) {
        const char *p = buffer + 5;
        const char *end = buffer + n;
        while (p < end && *p != ' ') p++;   /* HTTP version */
        while (p < end && *p == ' ') p++;
        if (p + 3 <= end) {
            int code = (p[0]-'0')*100 + (p[1]-'0')*10 + (p[2]-'0');
            int denied = (code == 200) ? 1 : 0;
            pthread_mutex_lock(&dl->mutex);
            dl->range_denied = denied;
            pthread_mutex_unlock(&dl->mutex);
        }
    }
    return n;
}

/* Progress callback: returning non-zero aborts the transfer. Used so
   stream_downloader_destroy() can stop a slow download promptly. Also
   captures the Content-Length from curl's dltotal (0 for chunked bodies)
   so callers can size the stream / compute seek byte offsets. */
static int dl_xferinfo_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t ultotal, curl_off_t ulnow) {
    (void)dlnow; (void)ultotal; (void)ulnow;
    StreamDownloader *dl = (StreamDownloader*)ud;
    int stop;
    pthread_mutex_lock(&dl->mutex);
    stop = dl->stop;
    if (dltotal > 0 && dl->total <= 0)
        dl->total = (int64_t)dltotal;
    pthread_mutex_unlock(&dl->mutex);
    return stop ? 1 : 0;
}
#endif

static void *dl_thread(void *arg) {
    StreamDownloader *dl = (StreamDownloader*)arg;
#ifdef HAVE_LIBCURL
    CURL *h = curl_easy_init();
    if (!h) {
        pthread_mutex_lock(&dl->mutex);
        dl->failed = 1;
        pthread_cond_broadcast(&dl->cond);
        pthread_mutex_unlock(&dl->mutex);
        return NULL;
    }
    /* Sequential downloader: "wb" (truncate/create). Resume/overwrite
       downloader (no_truncate): "r+b" — the file must already exist — and
       seek to start_byte (0 = from the beginning) so the fetched bytes land
       at the right offset without destroying the rest of the file. */
    FILE *fp;
    if (dl->no_truncate) {
        fp = fopen_utf8(dl->part_path, "r+b");
        if (fp && dl->start_byte > 0 &&
            fseek(fp, (long)dl->start_byte, SEEK_SET) != 0) {
            fclose(fp);
            fp = NULL;
        }
    } else {
        fp = fopen_utf8(dl->part_path, "wb");
    }
    if (!fp) {
        curl_easy_cleanup(h);
        LOG_ERROR("stream_downloader: cannot open .part %s (cache dir missing?)",
                  dl->part_path);
        pthread_mutex_lock(&dl->mutex);
        dl->failed = 1;
        pthread_cond_broadcast(&dl->cond);
        pthread_mutex_unlock(&dl->mutex);
        return NULL;
    }
    /* unbuffered: the playback thread reads this file concurrently and must
       see freshly written bytes immediately (no stdio buffering) */
    setvbuf(fp, NULL, _IONBF, 0);
#ifdef _WIN32
    mark_part_sparse(fp);   /* holes in the .part cost no disk until written */
#endif
    dl->fp = fp;

    curl_easy_setopt(h, CURLOPT_URL, dl->url);
    if (dl->start_byte > 0) {
        char range[32];
        snprintf(range, sizeof range, "%lld-", (long long)dl->start_byte);
        curl_easy_setopt(h, CURLOPT_RANGE, range);
        /* detect a server that ignores Range (200 full body) so the write
           callback can abort instead of corrupting the resume offset */
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, dl_range_status_cb);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, dl);
    }
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 8L);
    /* keep the connect timeout modest: stream_downloader_destroy() joins
       this thread, and a download stuck in connect can only be aborted via
       the progress callback below once the transfer is running — a shorter
       connect window bounds how long a track switch can stall */
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 8L);
    /* a transfer crawling below 1 B/s for 10 s is effectively dead — abort
       it so playback surfaces an error instead of hanging on the watermark */
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 10L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, dl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, dl);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, dl_xferinfo_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, dl);

    CURLcode rc = curl_easy_perform(h);
    curl_easy_cleanup(h);
    fclose(fp);
    dl->fp = NULL;

    pthread_mutex_lock(&dl->mutex);
    if (dl->stop) {
        /* aborted by destroy() — not a failure, not a clean finish */
        dl->done = 0;
        dl->failed = 0;
    } else {
        dl->done = 1;
        dl->failed = (rc != CURLE_OK);
        if (rc != CURLE_OK)
            LOG_ERROR("dl_thread curl failed (%d): %s", rc,
                      curl_easy_strerror(rc));
    }
    pthread_cond_broadcast(&dl->cond);
    pthread_mutex_unlock(&dl->mutex);
#else
    /* no libcurl: nothing to download — mark failed so the caller's
       wait_fn returns an error and playback does not hang */
    pthread_mutex_lock(&dl->mutex);
    dl->failed = 1;
    pthread_cond_broadcast(&dl->cond);
    pthread_mutex_unlock(&dl->mutex);
#endif
    return NULL;
}

StreamDownloader *stream_downloader_create(const char *url, const char *part_path) {
    if (!url || !url[0] || !part_path || !part_path[0]) return NULL;
    StreamDownloader *dl = (StreamDownloader*)calloc(1, sizeof(StreamDownloader));
    if (!dl) return NULL;
    dl->url = strdup(url);
    dl->part_path = strdup(part_path);
    if (!dl->url || !dl->part_path) {
        free(dl->url);
        free(dl->part_path);
        free(dl);
        return NULL;
    }
    pthread_mutex_init(&dl->mutex, NULL);
    /* Bind the condvar to the monotonic clock so timed waits are immune to
       wall-clock jumps (see wait_watermark). The MSVC pthread shim has no
       condattr/clock support — Windows falls back to the default attrs. */
#ifdef _WIN32
    pthread_cond_init(&dl->cond, NULL);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&dl->cond, &attr);
    pthread_condattr_destroy(&attr);
#endif
    return dl;
}

StreamDownloader *stream_downloader_create_resume(const char *url,
                                                  const char *part_path,
                                                  int64_t start_byte) {
    if (start_byte < 0) return NULL;
    StreamDownloader *dl = stream_downloader_create(url, part_path);
    if (dl) {
        dl->start_byte = start_byte;
        dl->no_truncate = 1;
    }
    return dl;
}

/* Non-blocking stop request: asks the download thread to abort (same flag
   stream_downloader_destroy sets) but does NOT join or free the object. Used
   to retire a redundant downloader (e.g. the parallel Range downloader once
   the sequential one has covered its region) without stalling the playback
   thread on a slow abort; the actual join/free happens later in destroy(),
   by which time the thread has already exited. */
void stream_downloader_stop(StreamDownloader *dl) {
    if (!dl) return;
    pthread_mutex_lock(&dl->mutex);
    dl->stop = 1;
    pthread_cond_broadcast(&dl->cond);
    pthread_mutex_unlock(&dl->mutex);
}

int stream_downloader_stopped(const StreamDownloader *dl) {
    if (!dl) return 0;
    int s;
    pthread_mutex_lock(&((StreamDownloader*)dl)->mutex);
    s = dl->stop;
    pthread_mutex_unlock(&((StreamDownloader*)dl)->mutex);
    return s;
}

int stream_downloader_write_errno(const StreamDownloader *dl) {
    if (!dl) return 0;
    int e;
    pthread_mutex_lock(&((StreamDownloader*)dl)->mutex);
    e = dl->write_errno;
    pthread_mutex_unlock(&((StreamDownloader*)dl)->mutex);
    return e;
}

int stream_downloader_start(StreamDownloader *dl) {
    if (!dl || dl->started) return -1;
    if (pthread_create(&dl->thread, NULL, dl_thread, dl) != 0) return -1;
    dl->started = 1;
    return 0;
}

int64_t stream_downloader_watermark(StreamDownloader *dl) {
    if (!dl) return 0;
    int64_t wm;
    pthread_mutex_lock(&dl->mutex);
    wm = dl->watermark;
    pthread_mutex_unlock(&dl->mutex);
    return wm;
}

int64_t stream_downloader_start_byte(const StreamDownloader *dl) {
    if (!dl) return 0;
    int64_t sb;
    pthread_mutex_lock(&((StreamDownloader*)dl)->mutex);
    sb = dl->start_byte;
    pthread_mutex_unlock(&((StreamDownloader*)dl)->mutex);
    return sb;
}

int64_t stream_downloader_total_size(const StreamDownloader *dl) {
    if (!dl) return 0;
    int64_t t;
    pthread_mutex_lock(&dl->mutex);
    t = dl->total;
    pthread_mutex_unlock(&dl->mutex);
    return t;
}

int stream_downloader_done(StreamDownloader *dl) {
    if (!dl) return 0;
    int d;
    pthread_mutex_lock(&dl->mutex);
    d = dl->done;
    pthread_mutex_unlock(&dl->mutex);
    return d;
}

int stream_downloader_failed(StreamDownloader *dl) {
    if (!dl) return 0;
    int f;
    pthread_mutex_lock(&dl->mutex);
    f = dl->failed;
    pthread_mutex_unlock(&dl->mutex);
    return f;
}

int stream_downloader_wait_watermark(StreamDownloader *dl, int64_t min_bytes,
                                     int timeout_ms) {
    if (!dl) return -1;
    pthread_mutex_lock(&dl->mutex);
    int64_t start = now_mono_ms();
    int64_t last_wm = dl->watermark;
    for (;;) {
        if (dl->stop) { pthread_mutex_unlock(&dl->mutex); return -1; }
        if (dl->failed) { pthread_mutex_unlock(&dl->mutex); return -1; }
        if (dl->done)   { pthread_mutex_unlock(&dl->mutex); return 1; }
        if (min_bytes >= 0 && dl->watermark >= min_bytes) {
            pthread_mutex_unlock(&dl->mutex); return 1;
        }
        if (min_bytes < 0 && dl->watermark > last_wm) {
            pthread_mutex_unlock(&dl->mutex); return 1;
        }
        if (timeout_ms >= 0) {
            int64_t elapsed = now_mono_ms() - start;
            if (elapsed >= timeout_ms) {
                pthread_mutex_unlock(&dl->mutex); return 0;
            }
            int64_t remain = timeout_ms - elapsed;
#ifdef _WIN32
            /* The MSVC pthread shim exposes no pthread_cond_timedwait (the
               Win32 CONDITION_VARIABLE has no timed-wait wrapper here), so
               poll instead: release the lock, sleep briefly, and let the
               loop re-check the conditions. */
            {
                DWORD ms = remain > 100 ? 100 : (DWORD)remain;
                pthread_mutex_unlock(&dl->mutex);
                Sleep(ms);
                pthread_mutex_lock(&dl->mutex);
            }
#else
            /* the condvar is created with CLOCK_MONOTONIC (see create), so
               the absolute deadline must use the monotonic clock too —
               using CLOCK_REALTIME here would make a wall-clock jump (NTP)
               stretch or shorten the wait */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += remain * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += ts.tv_nsec / 1000000000;
                ts.tv_nsec %= 1000000000;
            }
            pthread_cond_timedwait(&dl->cond, &dl->mutex, &ts);
#endif
        } else {
            pthread_cond_wait(&dl->cond, &dl->mutex);
        }
    }
}

void stream_downloader_destroy(StreamDownloader *dl) {
    if (!dl) return;
    if (dl->started) {
        pthread_mutex_lock(&dl->mutex);
        dl->stop = 1;
        pthread_cond_broadcast(&dl->cond);
        pthread_mutex_unlock(&dl->mutex);
        pthread_join(dl->thread, NULL);
    }
    pthread_mutex_destroy(&dl->mutex);
    pthread_cond_destroy(&dl->cond);
    free(dl->url);
    free(dl->part_path);
    free(dl);
}
