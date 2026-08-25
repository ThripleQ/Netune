#include "core/stream_downloader.h"
#include "infra/log.h"
#include "compat/utf8.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

struct StreamDownloader {
    char        *url;
    char        *part_path;
    pthread_t    thread;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    FILE        *fp;          /* write handle, owned by the download thread */
    int64_t      watermark;   /* bytes written so far */
    int          started;
    int          done;        /* download reached EOF cleanly */
    int          failed;      /* network error / aborted */
    int          stop;        /* request the download thread to abort */
};

static int64_t now_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#ifdef HAVE_LIBCURL
/* Write callback: append bytes to the .part file and advance the watermark.
   Runs on the download thread. */
static size_t dl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    StreamDownloader *dl = (StreamDownloader*)ud;
    size_t n = size * nmemb;
    size_t w = fwrite(ptr, 1, n, dl->fp);
    pthread_mutex_lock(&dl->mutex);
    dl->watermark += (int64_t)w;
    pthread_cond_broadcast(&dl->cond);
    pthread_mutex_unlock(&dl->mutex);
    return w;
}

/* Progress callback: returning non-zero aborts the transfer. Used so
   stream_downloader_destroy() can stop a slow download promptly. */
static int dl_xferinfo_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    StreamDownloader *dl = (StreamDownloader*)ud;
    int stop;
    pthread_mutex_lock(&dl->mutex);
    stop = dl->stop;
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
    FILE *fp = fopen_utf8(dl->part_path, "wb");
    if (!fp) {
        curl_easy_cleanup(h);
        pthread_mutex_lock(&dl->mutex);
        dl->failed = 1;
        pthread_cond_broadcast(&dl->cond);
        pthread_mutex_unlock(&dl->mutex);
        return NULL;
    }
    /* unbuffered: the playback thread reads this file concurrently and must
       see freshly written bytes immediately (no stdio buffering) */
    setvbuf(fp, NULL, _IONBF, 0);
    dl->fp = fp;

    curl_easy_setopt(h, CURLOPT_URL, dl->url);
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
    /* bind the condvar to the monotonic clock so timed waits are immune to
       wall-clock jumps (see wait_watermark) */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&dl->cond, &attr);
    pthread_condattr_destroy(&attr);
    return dl;
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
