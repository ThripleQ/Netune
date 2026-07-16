#pragma once
/*
 * Minimal pthread subset for MSVC Windows, backed by Win32 APIs.
 *
 * Provides: pthread_t, pthread_mutex_t, pthread_cond_t,
 *           create/join/detach, mutex init/destroy/lock/unlock,
 *           condvar init/destroy/wait/signal/broadcast.
 *
 * Only compiled on MSVC; POSIX platforms use the real <pthread.h>.
 */

#ifdef _MSC_VER

#include <stdlib.h>
#include <windows.h>

/* ── pthread_t ──────────────────────────────────────── */
typedef HANDLE pthread_t;

static inline int pthread_create(pthread_t *thread, void *attr,
                                 void *(*start)(void *), void *arg)
{
    (void)attr;
    *thread = CreateThread(NULL, 0,
        (LPTHREAD_START_ROUTINE)start, arg, 0, NULL);
    return *thread ? 0 : -1;
}

static inline int pthread_join(pthread_t thread, void **retval)
{
    (void)retval;
    DWORD r = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return r == WAIT_OBJECT_0 ? 0 : -1;
}

static inline int pthread_detach(pthread_t thread)
{
    return CloseHandle(thread) ? 0 : -1;
}

/* ── pthread_mutex_t (CRITICAL_SECTION) ─────────────── */
typedef CRITICAL_SECTION pthread_mutex_t;
typedef void pthread_mutexattr_t;

static inline int pthread_mutex_init(pthread_mutex_t *m, void *attr)
{
    (void)attr;
    InitializeCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    DeleteCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    EnterCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    LeaveCriticalSection(m);
    return 0;
}

/* ── pthread_cond_t (Windows CONDITION_VARIABLE, Vista+) ── */
typedef CONDITION_VARIABLE pthread_cond_t;
typedef void pthread_condattr_t;

static inline int pthread_cond_init(pthread_cond_t *c, void *attr)
{
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0; /* Win32 CVs don't need explicit destroy */
}
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return SleepConditionVariableCS(c, m, INFINITE) ? 0 : -1;
}
static inline int pthread_cond_signal(pthread_cond_t *c)
{
    WakeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *c)
{
    WakeAllConditionVariable(c);
    return 0;
}

#endif /* _MSC_VER */
