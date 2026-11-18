#include "os.h"
#include "spsc.h"

#include <assert.h>
#include <string.h>

static usize CheckedWrite(spsc_buffer *buffer, const_byte_slice input)
{
    spsc_write_result result = SpscWrite(buffer, input);
    assert(result.tag == SPSC_WRITE_OK);
    return result.value;
}
static usize CheckedRead(spsc_buffer *buffer, byte_slice output)
{
    spsc_read_result result = SpscRead(buffer, output);
    assert(result.tag == SPSC_READ_OK);
    return result.value;
}
static void TestBoundaries(void)
{
    u8 storage[8];
    spsc_buffer buffer;
    byte_slice ring = {storage, sizeof(storage)};
    assert(SpscInit(NULL, ring).tag == SPSC_INIT_INVALID_ARGUMENT);
    assert(SpscInit(&buffer, (byte_slice){0}).tag == SPSC_INIT_INVALID_ARGUMENT);
    assert(SpscInit(&buffer, (byte_slice){storage, 3}).tag == SPSC_INIT_INVALID_ARGUMENT);
    assert(SpscInit(&buffer, (byte_slice){storage, (usize)PTRDIFF_MAX + 1}).tag == SPSC_INIT_INVALID_ARGUMENT);
    assert(SpscInit(&buffer, ring).tag == SPSC_INIT_OK);
    assert(CheckedWrite(&buffer, (const_byte_slice){0}) == 0);
    assert(CheckedRead(&buffer, (byte_slice){0}) == 0);
    assert(SpscWrite(&buffer, (const_byte_slice){NULL, 1}).tag == SPSC_WRITE_INVALID_ARGUMENT);
    assert(SpscRead(&buffer, (byte_slice){NULL, 1}).tag == SPSC_READ_INVALID_ARGUMENT);

    u8 input_storage[16];
    byte_slice input = {input_storage, sizeof(input_storage)};
    SliceFor(input, index) {
        input.ptr[index] = (u8)index;
    }
    u8 output_storage[8];
    byte_slice output = {output_storage, sizeof(output_storage)};
    assert(CheckedRead(&buffer, output) == 0);
    assert(CheckedWrite(&buffer, (const_byte_slice){input.ptr, input.len}) == 8);
    assert(CheckedWrite(&buffer, (const_byte_slice){input.ptr, 1}) == 0);
    assert(CheckedRead(&buffer, (byte_slice){output.ptr, 3}) == 3);
    assert(memcmp(output.ptr, input.ptr, 3) == 0);
    assert(CheckedWrite(&buffer, (const_byte_slice){input.ptr + 8, 8}) == 3);
    assert(CheckedRead(&buffer, output) == 8);
    assert(memcmp(output.ptr, input.ptr + 3, 8) == 0);
    assert(CheckedRead(&buffer, output) == 0);

    /* White-box rollover check with exclusive ownership, before any threads. */
    usize near_wrap = SIZE_MAX - 3;
    atomic_store(&buffer.producer.position, near_wrap);
    atomic_store(&buffer.consumer.position, near_wrap);
    buffer.producer.cached_peer = near_wrap;
    buffer.consumer.cached_peer = near_wrap;
    assert(CheckedWrite(&buffer, (const_byte_slice){input.ptr, 8}) == 8);
    assert(CheckedRead(&buffer, output) == 8);
    assert(memcmp(output.ptr, input.ptr, 8) == 0);
    assert(atomic_load(&buffer.producer.position) == 4);
    assert(atomic_load(&buffer.consumer.position) == 4);
}

static u8 Pattern(usize offset)
{
    u64 value = (u64)offset * UINT64_C(0x9e3779b97f4a7c15);
    return (u8)(value ^ (value >> 17) ^ (value >> 41));
}

static void WriteAll(spsc_buffer *buffer, const_byte_slice bytes)
{
    while (bytes.len != 0) {
        usize count = CheckedWrite(buffer, bytes);
        if (count == 0) {
            ThreadYield();
            continue;
        }
        bytes.ptr += count;
        bytes.len -= (usize)count;
    }
}

typedef struct transfer_context {
    spsc_buffer *input;
    spsc_buffer *output;
    usize total;
    usize transferred;
} transfer_context;

static void Produce(void *argument)
{
    transfer_context *context = argument;
    u8 storage[733];
    byte_slice batch = {storage, sizeof(storage)};
    while (context->transferred < context->total) {
        usize remaining = context->total - context->transferred;
        byte_slice chunk = {batch.ptr, remaining < batch.len ? remaining : batch.len};
        SliceFor(chunk, index) {
            chunk.ptr[index] = Pattern(context->transferred + index);
        }
        WriteAll(context->output, (const_byte_slice){chunk.ptr, chunk.len});
        context->transferred += chunk.len;
    }
}

static void Relay(void *argument)
{
    transfer_context *context = argument;
    u8 storage[503];
    byte_slice batch = {storage, sizeof(storage)};
    while (context->transferred < context->total) {
        usize count = CheckedRead(context->input, batch);
        if (count == 0) {
            ThreadYield();
            continue;
        }
        byte_slice chunk = {batch.ptr, (usize)count};
        SliceFor(chunk, index) {
            assert(chunk.ptr[index] == Pattern(context->transferred + index));
            chunk.ptr[index] ^= 0x5a;
        }
        WriteAll(context->output, (const_byte_slice){chunk.ptr, chunk.len});
        context->transferred += chunk.len;
    }
}

static void TestThreads(usize capacity, usize total)
{
    u8 request_storage[4096];
    u8 reply_storage[4096];
    assert(capacity <= sizeof(request_storage));
    spsc_buffer requests;
    spsc_buffer replies;
    assert(SpscInit(&requests, (byte_slice){request_storage, capacity}).tag == SPSC_INIT_OK);
    assert(SpscInit(&replies, (byte_slice){reply_storage, capacity}).tag == SPSC_INIT_OK);
    transfer_context producer = {.output = &requests, .total = total};
    transfer_context relay = {.input = &requests, .output = &replies, .total = total};
    os_thread *producer_thread;
    os_thread *relay_thread;
    assert(ThreadStart(NULL, &producer, (os_cpu_id_slice){0}, &producer_thread).tag == THREAD_START_INVALID_ARGUMENT);
    assert(producer_thread == NULL);
    assert(ThreadStart(Produce, &producer, (os_cpu_id_slice){0}, NULL).tag == THREAD_START_INVALID_ARGUMENT);
    assert(ThreadJoin(NULL).tag == THREAD_JOIN_INVALID_ARGUMENT);
    assert(ThreadStart(Produce, &producer, (os_cpu_id_slice){0}, &producer_thread).tag == THREAD_START_OK);
    assert(ThreadStart(Relay, &relay, (os_cpu_id_slice){0}, &relay_thread).tag == THREAD_START_OK);

    u8 storage[977];
    byte_slice batch = {storage, sizeof(storage)};
    usize received = 0;
    while (received < total) {
        usize count = CheckedRead(&replies, batch);
        assert((usize)count <= total - received);
        if (count == 0) {
            ThreadYield();
            continue;
        }
        byte_slice chunk = {batch.ptr, (usize)count};
        SliceFor(chunk, index) {
            assert(chunk.ptr[index] == (u8)(Pattern(received + index) ^ 0x5a));
        }
        received += chunk.len;
    }
    assert(ThreadJoin(producer_thread).tag == THREAD_JOIN_OK);
    assert(ThreadJoin(relay_thread).tag == THREAD_JOIN_OK);
    /* Join makes each thread's private progress visible to the owner. */
    assert(producer.transferred == total && relay.transferred == total);
    assert(CheckedRead(&requests, batch) == 0);
    assert(CheckedRead(&replies, batch) == 0);
}

int main(void)
{
    TestBoundaries();
    TestThreads(1, 65537);
    TestThreads(64, 1024 * 1024 + 19);
    TestThreads(4096, 8 * 1024 * 1024 + 37);
    TerminalWrite(StringSlice("PASS: SPSC full/empty, partial transfers, rollover, threaded ordering\n"));
    return 0;
}
