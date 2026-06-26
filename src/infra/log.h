#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Log levels ─────────────────────────────────────── */
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/* ── API ────────────────────────────────────────────── */
void log_init(const char *file);
void log_shutdown(void);
void log_write(LogLevel level, const char *file, int line,
               const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* ── Convenience macros ─────────────────────────────── */
#define LOG_DEBUG(fmt, ...) log_write(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_write(LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_write(LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_write(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
