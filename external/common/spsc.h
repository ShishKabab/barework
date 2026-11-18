#ifndef COMMON_SPSC_H
#define COMMON_SPSC_H
#include "spsc_results.h"

#include "types.h"

#include <stdatomic.h>

typedef struct spsc_cursor {
    _Atomic usize position;
    usize cached_peer;
} spsc_cursor;

/* Caller-owned byte stream, not a message queue. Fields are implementation
 * state: never modify/copy them after initialization while either side runs.
 * Exactly one thread writes and one thread reads. Storage must not overlap
 * this object or input/output slices and must outlive both participants.
 * Keep the object's alignment when allocating it dynamically (aligned_alloc).
 * The 64-byte separation targets common cache lines; it is not a universal
 * hardware cache-line guarantee. */
typedef struct spsc_buffer {
    byte_slice storage;
    _Alignas(64) spsc_cursor producer;
    _Alignas(64) spsc_cursor consumer;
} spsc_buffer;

/* Initialize before publishing to threads. Capacity must be a nonzero power
 * of two, at most PTRDIFF_MAX. Returns a tagged result; rejects targets
 * without lock-free usize atomics. Reinitialize only after both sides stop. */
spsc_init_result SpscInit(spsc_buffer *buffer, byte_slice storage);

/* Nonblocking: OK carries bytes copied, zero for full/empty (or empty input).
 * INVALID_ARGUMENT has no count payload. Transfers may be partial. A write
 * publishes its bytes together; reads may split/combine writes. Each call
 * performs at most two copies and one publication; no allocation or locks.
 * These require successful initialization. Use one buffer per direction.
 * There is no implicit EOF, cancellation, or wakeup: agree on a byte count or
 * an application-level end message, and choose a retry/wakeup policy outside
 * this API. Do not release storage until both threads have stopped/joined. */
spsc_write_result SpscWrite(spsc_buffer *buffer, const_byte_slice input);
spsc_read_result SpscRead(spsc_buffer *buffer, byte_slice output);

#endif
