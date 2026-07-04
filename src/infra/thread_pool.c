#include "infra/thread_pool.h"
#include "infra/log.h"
#include <stdlib.h>
#include <pthread.h>

/* ── Task node (linked list) ─────────────────────────── */
typedef struct task_node {
    threadpool_task_fn  fn;
    void               *arg;
    struct task_node   *next;
} task_node_t;

/* ── Pool ────────────────────────────────────────────── */
struct threadpool {
    pthread_t      *workers;
    int             num_threads;

    task_node_t    *head;           /* dequeue from head */
    task_node_t    *tail;           /* enqueue at tail   */
    int             pending;        /* queued task count */

    pthread_mutex_t mutex;
    pthread_cond_t  cond;           /* signalled when work arrives */

    volatile int    shutdown;       /* 1 → workers should exit */
};

/* ── Worker routine ──────────────────────────────────── */
static void* worker_loop(void *arg) {
    threadpool_t *tp = (threadpool_t*)arg;

    while (1) {
        pthread_mutex_lock(&tp->mutex);

        /* wait for work or shutdown */
        while (tp->pending == 0 && !tp->shutdown)
            pthread_cond_wait(&tp->cond, &tp->mutex);

        /* shutdown + no work → exit */
        if (tp->shutdown && tp->pending == 0) {
            pthread_mutex_unlock(&tp->mutex);
            break;
        }

        /* dequeue one task */
        task_node_t *node = tp->head;
        tp->head = node->next;
        if (!tp->head) tp->tail = NULL;
        tp->pending--;

        pthread_mutex_unlock(&tp->mutex);

        /* execute */
        node->fn(node->arg);
        free(node);
    }

    return NULL;
}

/* ── Create ──────────────────────────────────────────── */
threadpool_t* threadpool_create(int num_threads) {
    if (num_threads < 1) num_threads = 1;
    if (num_threads > 64) num_threads = 64;

    threadpool_t *tp = (threadpool_t*)calloc(1, sizeof(threadpool_t));
    if (!tp) return NULL;

    tp->num_threads = num_threads;
    tp->head        = NULL;
    tp->tail        = NULL;
    tp->pending     = 0;
    tp->shutdown    = 0;

    pthread_mutex_init(&tp->mutex, NULL);
    pthread_cond_init(&tp->cond, NULL);

    tp->workers = (pthread_t*)malloc((size_t)num_threads * sizeof(pthread_t));
    if (!tp->workers) {
        pthread_cond_destroy(&tp->cond);
        pthread_mutex_destroy(&tp->mutex);
        free(tp);
        return NULL;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&tp->workers[i], NULL, worker_loop, tp) != 0) {
            LOG_ERROR("Failed to create worker thread %d/%d", i + 1, num_threads);
            /* shut down any workers already created */
            tp->shutdown = 1;
            pthread_cond_broadcast(&tp->cond);
            for (int j = 0; j < i; j++)
                pthread_join(tp->workers[j], NULL);
            pthread_cond_destroy(&tp->cond);
            pthread_mutex_destroy(&tp->mutex);
            free(tp->workers);
            free(tp);
            return NULL;
        }
    }

    LOG_INFO("Thread pool created with %d workers", num_threads);
    return tp;
}

/* ── Submit ──────────────────────────────────────────── */
int threadpool_submit(threadpool_t *tp, threadpool_task_fn fn, void *arg) {
    if (!tp || !fn) return -1;

    pthread_mutex_lock(&tp->mutex);

    if (tp->shutdown) {
        pthread_mutex_unlock(&tp->mutex);
        return -1;
    }

    task_node_t *node = (task_node_t*)malloc(sizeof(task_node_t));
    if (!node) {
        pthread_mutex_unlock(&tp->mutex);
        return -1;
    }

    node->fn   = fn;
    node->arg  = arg;
    node->next = NULL;

    if (!tp->tail) {
        tp->head = tp->tail = node;
    } else {
        tp->tail->next = node;
        tp->tail = node;
    }
    tp->pending++;

    pthread_cond_signal(&tp->cond);
    pthread_mutex_unlock(&tp->mutex);

    return 0;
}

/* ── Destroy ─────────────────────────────────────────── */
void threadpool_destroy(threadpool_t *tp) {
    if (!tp) return;

    pthread_mutex_lock(&tp->mutex);

    /* wait for all pending tasks to drain */
    while (tp->pending > 0)
        pthread_cond_wait(&tp->cond, &tp->mutex);

    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->cond);
    pthread_mutex_unlock(&tp->mutex);

    for (int i = 0; i < tp->num_threads; i++)
        pthread_join(tp->workers[i], NULL);

    LOG_INFO("Thread pool destroyed (%d workers)", tp->num_threads);

    pthread_cond_destroy(&tp->cond);
    pthread_mutex_destroy(&tp->mutex);
    free(tp->workers);
    free(tp);
}
