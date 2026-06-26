#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

static FILE      *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str(LogLevel level) {
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "????";
    }
}

void log_init(const char *path) {
    if (path) {
        g_log_file = fopen(path, "a");
        if (!g_log_file) {
            fprintf(stderr, "[LOG] cannot open log file: %s\n", path);
        }
    }
    LOG_INFO("Logger initialized");
}

void log_shutdown(void) {
    if (g_log_file) {
        LOG_INFO("Logger shutting down");
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_write(LogLevel level, const char *file, int line,
               const char *fmt, ...) {
    pthread_mutex_lock(&g_log_mutex);

    /* timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

    /* pick output */
    FILE *out = g_log_file ? g_log_file : stderr;

    fprintf(out, "[%s] %-5s ", ts, level_str(level));
    if (file) fprintf(out, "%s:%d ", file, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fprintf(out, "\n");
    fflush(out);

    pthread_mutex_unlock(&g_log_mutex);
}
