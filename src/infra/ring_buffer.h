#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* ── Opaque type ─────────────────────────────────────── */
typedef struct ringbuffer ringbuffer_t;

/* ── API ─────────────────────────────────────────────── */
ringbuffer_t* ringbuffer_create(size_t capacity);
void          ringbuffer_destroy(ringbuffer_t *rb);

/* blocking write — returns bytes written (may be < len only on destroy) */
size_t ringbuffer_write(ringbuffer_t *rb, const void *data, size_t len);

/* blocking read — returns bytes read (0 = closed/destroyed) */
size_t ringbuffer_read(ringbuffer_t *rb, void *buf, size_t len);

/* non-blocking queries */
size_t ringbuffer_available(const ringbuffer_t *rb);
size_t ringbuffer_free_space(const ringbuffer_t *rb);

/* discard all buffered data and wake blocked readers/writers */
void ringbuffer_reset(ringbuffer_t *rb);

#ifdef __cplusplus
}
#endif
