#include "spsc.h"

#include <string.h>

spsc_init_result SpscInit(spsc_buffer *buffer, byte_slice storage)
{
    if (buffer == NULL || storage.ptr == NULL || storage.len == 0 ||
        storage.len > PTRDIFF_MAX || (storage.len & (storage.len - 1)) != 0) {
        return (spsc_init_result){.tag = SPSC_INIT_INVALID_ARGUMENT};
    }
    buffer->storage = storage;
    atomic_init(&buffer->producer.position, 0);
    atomic_init(&buffer->consumer.position, 0);
    buffer->producer.cached_peer = 0;
    buffer->consumer.cached_peer = 0;
    if (!atomic_is_lock_free(&buffer->producer.position) ||
        !atomic_is_lock_free(&buffer->consumer.position)) {
        return (spsc_init_result){.tag = SPSC_INIT_NOT_SUPPORTED};
    }
    return (spsc_init_result){.tag = SPSC_INIT_OK};
}

spsc_write_result SpscWrite(spsc_buffer *buffer, const_byte_slice input)
{
    if (buffer == NULL || (input.ptr == NULL && input.len != 0)) {
        return (spsc_write_result){.tag = SPSC_WRITE_INVALID_ARGUMENT};
    }
    if (input.len == 0) {
        return (spsc_write_result){.tag = SPSC_WRITE_OK};
    }
    usize capacity = buffer->storage.len;
    usize count = input.len < capacity ? input.len : capacity;
    usize position = atomic_load_explicit(&buffer->producer.position, memory_order_relaxed);
    usize available = capacity - (position - buffer->producer.cached_peer);
    if (available < count) {
        /* Acquire ensures the consumer finished reading before we reuse bytes. */
        buffer->producer.cached_peer = atomic_load_explicit(
            &buffer->consumer.position, memory_order_acquire);
        available = capacity - (position - buffer->producer.cached_peer);
    }
    if (count > available) {
        count = available;
    }
    if (count == 0) {
        return (spsc_write_result){.tag = SPSC_WRITE_OK};
    }
    usize offset = position & (capacity - 1);
    usize first = count < capacity - offset ? count : capacity - offset;
    memcpy(buffer->storage.ptr + offset, input.ptr, first);
    if (first != count) {
        memcpy(buffer->storage.ptr, input.ptr + first, count - first);
    }
    /* Unsigned counters wrap safely; power-of-two capacity preserves indexing. */
    atomic_store_explicit(&buffer->producer.position, position + count, memory_order_release);
    return (spsc_write_result){.tag = SPSC_WRITE_OK, .value = count};
}

spsc_read_result SpscRead(spsc_buffer *buffer, byte_slice output)
{
    if (buffer == NULL || (output.ptr == NULL && output.len != 0)) {
        return (spsc_read_result){.tag = SPSC_READ_INVALID_ARGUMENT};
    }
    if (output.len == 0) {
        return (spsc_read_result){.tag = SPSC_READ_OK};
    }
    usize capacity = buffer->storage.len;
    usize count = output.len < capacity ? output.len : capacity;
    usize position = atomic_load_explicit(&buffer->consumer.position, memory_order_relaxed);
    usize available = buffer->consumer.cached_peer - position;
    if (available < count) {
        /* Acquire makes the producer's byte copies visible before reading. */
        buffer->consumer.cached_peer = atomic_load_explicit(
            &buffer->producer.position, memory_order_acquire);
        available = buffer->consumer.cached_peer - position;
    }
    if (count > available) {
        count = available;
    }
    if (count == 0) {
        return (spsc_read_result){.tag = SPSC_READ_OK};
    }
    usize offset = position & (capacity - 1);
    usize first = count < capacity - offset ? count : capacity - offset;
    memcpy(output.ptr, buffer->storage.ptr + offset, first);
    if (first != count) {
        memcpy(output.ptr + first, buffer->storage.ptr, count - first);
    }
    atomic_store_explicit(&buffer->consumer.position, position + count, memory_order_release);
    return (spsc_read_result){.tag = SPSC_READ_OK, .value = count};
}
