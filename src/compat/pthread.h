#pragma once
/*
 * Minimal pthread subset for MSVC Windows, backed by Win32 APIs.
 *
 * Provides: pthread_t, pthread_mutex_t, pthread_cond_t,
 *           create/join/detach, mutex init/destroy/lock/unlock,
 *           condvar init/destroy/wait/signal/broadcast.
 *
 * Uses SRWLOCK for mutexes (statically initialisable → PTHREAD_MUTEX_INITIALIZER
 * works without a runtime Init call, matching POSIX semantics for static mutexes).
 * Uses CONDITION_VARIABLE for condvars (Vista+, also zero-initialisable).
 *
 * Only compiled on MSVC; POSIX platforms use the real <pthread.h>.
 */

#ifdef _MSC_VER

#include <stdlib.h>
#include <windows.h>

/* ── pthread_t ──────────────────────────────────────── */
typedef HANDLE pthread_t;

/* Wrapper that adapts the pthread start signature (void* fn(void*))
   to the Win32 thread proc signature (DWORD fn(LPVOID)). This avoids
   the undefined behaviour of casting an incompatible function pointer
   to LPTHREAD_START_ROUTINE. */
typedef struct {
    void *(*start_routine)(void *);
    void  *arg;
} pthread_start_wrap;

static DWORD WINAPI pthread_start_trampoline(LPVOID p)
{
    pthread_start_wrap *w = (pthread_start_wrap *)p;
    void *(*fn)(void *) = w->start_routine;
    void *arg = w->arg;
    free(w);
    fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attr,
                                 void *(*start)(void *), void *arg)
{
    (void)attr;
    pthread_start_wrap *w = (pthread_start_wrap *)malloc(sizeof(*w));
    if (!w) return -1;
    w->start_routine = start;
    w->arg = arg;
    *thread = CreateThread(NULL, 0, pthread_start_trampoline, w, 0, NULL);
    if (!*thread) { free(w); return -1; }
    return 0;
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

/* ── pthread_mutex_t (SRWLOCK — zero-initialisable) ──
 * SRWLOCK is an exclusive/acquired lock that can be statically
 * initialised to {0} (SRWLOCK_INIT), so PTHREAD_MUTEX_INITIALIZER
 * just works without a runtime Init call. It is non-recursive, which
 * matches the default PTHREAD_MUTEX_DEFAULT on most POSIX systems. */
typedef SRWLOCK pthread_mutex_t;
typedef void    pthread_mutexattr_t;

/* SRWLOCK_INIT is {0} on all supported Windows versions; {0} is also a
   valid zero-initialiser for the struct. This lets static mutex variables
   work the same as POSIX PTHREAD_MUTEX_INITIALIZER. */
#define PTHREAD_MUTEX_INITIALIZER { 0 }

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    InitializeSRWLock(m);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0; /* SRWLOCK needs no explicit destroy */
}
static inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    AcquireSRWLockExclusive(m);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    ReleaseSRWLockExclusive(m);
    return 0;
}

/* ── pthread_cond_t (Windows CONDITION_VARIABLE, Vista+) ──
 * CONDITION_VARIABLE is also zero-initialisable, so
 * PTHREAD_COND_INITIALIZER works for static condvars too. */
typedef CONDITION_VARIABLE pthread_cond_t;
typedef void               pthread_condattr_t;

#define PTHREAD_COND_INITIALIZER { 0 }

static inline int pthread_cond_init(pthread_cond_t *c, const void *attr)
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
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1;
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
