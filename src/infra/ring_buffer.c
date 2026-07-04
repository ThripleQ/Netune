#include "infra/ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

struct ringbuffer {
    uint8_t         *buf;
    size_t           capacity;
    size_t           head;     /* next read position  */
    size_t           tail;     /* next write position */
    size_t           count;    /* bytes currently stored */

    pthread_mutex_t  mutex;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;
    volatile int     closed;   /* set on destroy — unblocks threads */
};

ringbuffer_t* ringbuffer_create(size_t capacity) {
    if (capacity == 0) return NULL;

    ringbuffer_t *rb = (ringbuffer_t*)calloc(1, sizeof(ringbuffer_t));
    if (!rb) return NULL;

    rb->buf = (uint8_t*)malloc(capacity);
    if (!rb->buf) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->head     = 0;
    rb->tail     = 0;
    rb->count    = 0;
    rb->closed   = 0;

    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);

    return rb;
}

void ringbuffer_destroy(ringbuffer_t *rb) {
    if (!rb) return;

    pthread_mutex_lock(&rb->mutex);
    rb->closed = 1;
    pthread_cond_broadcast(&rb->not_empty);
    pthread_cond_broadcast(&rb->not_full);
    pthread_mutex_unlock(&rb->mutex);

    /* give blocked threads a moment to wake */
    pthread_mutex_lock(&rb->mutex);
    pthread_mutex_unlock(&rb->mutex);

    pthread_cond_destroy(&rb->not_full);
    pthread_cond_destroy(&rb->not_empty);
    pthread_mutex_destroy(&rb->mutex);

    free(rb->buf);
    free(rb);
}

/* ── helpers ────────────────────────────────────────── */
static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

size_t ringbuffer_write(ringbuffer_t *rb, const void *data, size_t len) {
    if (!rb || !data || len == 0) return 0;

    pthread_mutex_lock(&rb->mutex);

    /* wait until there is room or the buffer is closed */
    while (rb->count == rb->capacity && !rb->closed)
        pthread_cond_wait(&rb->not_full, &rb->mutex);

    if (rb->closed) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    /* how much we can actually write */
    size_t space = rb->capacity - rb->count;
    size_t to_write = min_size(len, space);

    /* two-part copy if wrap-around */
    size_t first = min_size(to_write, rb->capacity - rb->tail);
    memcpy(rb->buf + rb->tail, data, first);
    if (first < to_write) {
        memcpy(rb->buf, (const uint8_t*)data + first, to_write - first);
    }

    rb->tail = (rb->tail + to_write) % rb->capacity;
    rb->count += to_write;

    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mutex);

    return to_write;
}

size_t ringbuffer_read(ringbuffer_t *rb, void *buf, size_t len) {
    if (!rb || !buf || len == 0) return 0;

    pthread_mutex_lock(&rb->mutex);

    /* wait until data is available or the buffer is closed */
    while (rb->count == 0 && !rb->closed)
        pthread_cond_wait(&rb->not_empty, &rb->mutex);

    if (rb->count == 0 && rb->closed) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    size_t to_read = min_size(len, rb->count);

    /* two-part copy if wrap-around */
    size_t first = min_size(to_read, rb->capacity - rb->head);
    memcpy(buf, rb->buf + rb->head, first);
    if (first < to_read) {
        memcpy((uint8_t*)buf + first, rb->buf, to_read - first);
    }

    rb->head = (rb->head + to_read) % rb->capacity;
    rb->count -= to_read;

    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mutex);

    return to_read;
}

size_t ringbuffer_available(const ringbuffer_t *rb) {
    if (!rb) return 0;
    pthread_mutex_lock((pthread_mutex_t*)&rb->mutex);
    size_t n = rb->count;
    pthread_mutex_unlock((pthread_mutex_t*)&rb->mutex);
    return n;
}

size_t ringbuffer_free_space(const ringbuffer_t *rb) {
    if (!rb) return 0;
    pthread_mutex_lock((pthread_mutex_t*)&rb->mutex);
    size_t n = rb->capacity - rb->count;
    pthread_mutex_unlock((pthread_mutex_t*)&rb->mutex);
    return n;
}

void ringbuffer_reset(ringbuffer_t *rb) {
    if (!rb) return;
    pthread_mutex_lock(&rb->mutex);
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
    pthread_cond_broadcast(&rb->not_full);
    pthread_mutex_unlock(&rb->mutex);
}
