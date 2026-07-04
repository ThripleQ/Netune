#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque type ─────────────────────────────────────── */
typedef struct threadpool threadpool_t;

/* ── Task signature ──────────────────────────────────── */
typedef void (*threadpool_task_fn)(void *arg);

/* ── API ─────────────────────────────────────────────── */

/* Create a pool with `num_threads` worker threads.
   Returns NULL on failure. */
threadpool_t* threadpool_create(int num_threads);

/* Submit a task. Returns 0 on success, -1 if the pool is
   shutting down or full. The task will be executed by one
   of the worker threads. */
int threadpool_submit(threadpool_t *tp, threadpool_task_fn fn, void *arg);

/* Wait for all queued tasks to complete, then stop all
   workers and free resources. After destroy, no further
   submissions are accepted. */
void threadpool_destroy(threadpool_t *tp);

#ifdef __cplusplus
}
#endif
