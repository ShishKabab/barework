#include "worker.h"
#include "spsc.h"
#include "slice.h"
#include <stdlib.h>

typedef struct worker {
    spsc_buffer requests;
    spsc_buffer results;
    os_thread *thread;
    _Atomic i32 stop;
    _Atomic i32 broken;
    _Alignas(64) usize pending;
} worker;
typedef struct worker_slice { worker *ptr; usize len; } worker_slice;
struct worker_pool { worker_slice workers; usize capacity; };

static void WorkerEntry(void *context)
{
    worker *state = context;
    for (;;) {
        if (atomic_load_explicit(&state->stop, memory_order_acquire)) return;
        worker_job job;
        spsc_read_result read = SpscRead(&state->requests, (byte_slice){(u8 *)&job, sizeof(job)});
        if (read.tag != SPSC_READ_OK) break;
        if (read.value == 0) continue;
        if (read.value != sizeof(job)) break;
        worker_result result = job.function(job);
        result.id = job.id;
        spsc_write_result written = SpscWrite(&state->results, BytesOf(result));
        if (written.tag != SPSC_WRITE_OK || written.value != sizeof(result)) break;
    }
    atomic_store_explicit(&state->broken, 1, memory_order_release);
}

typedef enum worker_buffer_create_tag {
    WORKER_BUFFER_CREATE_OK, WORKER_BUFFER_CREATE_OVERFLOW,
    WORKER_BUFFER_CREATE_OUT_OF_MEMORY, WORKER_BUFFER_CREATE_NOT_SUPPORTED,
    WORKER_BUFFER_CREATE_INVALID_ARGUMENT,
} worker_buffer_create_tag;
typedef struct worker_buffer_create_result { worker_buffer_create_tag tag; } worker_buffer_create_result;
static worker_buffer_create_result WorkerBufferCreate(spsc_buffer *buffer, usize capacity, usize record_size)
{
    if (capacity > (usize)PTRDIFF_MAX / record_size)
        return (worker_buffer_create_result){.tag = WORKER_BUFFER_CREATE_OVERFLOW};
    usize bytes = 1;
    while (bytes < capacity * record_size) {
        if (bytes > (usize)PTRDIFF_MAX / 2) return (worker_buffer_create_result){.tag = WORKER_BUFFER_CREATE_OVERFLOW};
        bytes *= 2;
    }
    byte_slice storage = {malloc(bytes), bytes};
    if (storage.ptr == NULL) return (worker_buffer_create_result){.tag = WORKER_BUFFER_CREATE_OUT_OF_MEMORY};
    spsc_init_result initialized = SpscInit(buffer, storage);
    if (initialized.tag != SPSC_INIT_OK) {
        free(storage.ptr);
        buffer->storage = (byte_slice){0};
        return (worker_buffer_create_result){.tag = initialized.tag == SPSC_INIT_NOT_SUPPORTED ?
            WORKER_BUFFER_CREATE_NOT_SUPPORTED : WORKER_BUFFER_CREATE_INVALID_ARGUMENT};
    }
    return (worker_buffer_create_result){.tag = WORKER_BUFFER_CREATE_OK};
}

typedef enum worker_pool_release_tag { WORKER_POOL_RELEASE_OK, WORKER_POOL_RELEASE_SYSTEM_ERROR } worker_pool_release_tag;
typedef struct worker_pool_release_result { worker_pool_release_tag tag; i32 native_error; } worker_pool_release_result;
static worker_pool_release_result WorkerPoolRelease(worker_pool *pool)
{
    SliceFor(pool->workers, index) {
        atomic_store_explicit(&pool->workers.ptr[index].stop, 1, memory_order_release);
    }
    SliceFor(pool->workers, index) {
        worker *state = &pool->workers.ptr[index];
        if (state->thread != NULL) {
            thread_join_result joined = ThreadJoin(state->thread);
            if (joined.tag != THREAD_JOIN_OK)
                return (worker_pool_release_result){.tag = WORKER_POOL_RELEASE_SYSTEM_ERROR, .native_error = joined.native_error};
            state->thread = NULL;
        }
        free(state->requests.storage.ptr);
        free(state->results.storage.ptr);
    }
    free(pool->workers.ptr);
    free(pool);
    return (worker_pool_release_result){.tag = WORKER_POOL_RELEASE_OK};
}

worker_pool_create_result WorkerPoolCreate(usize count, usize capacity, os_cpu_id_slice placement, worker_pool **output)
{
    if (output == NULL) return (worker_pool_create_result){.tag = WORKER_POOL_CREATE_INVALID_ARGUMENT};
    *output = NULL;
    if (count == 0 || capacity == 0 || count > SIZE_MAX / sizeof(worker) ||
        (placement.ptr == NULL && placement.len != 0))
        return (worker_pool_create_result){.tag = WORKER_POOL_CREATE_INVALID_ARGUMENT};
    worker_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) return (worker_pool_create_result){.tag = WORKER_POOL_CREATE_OUT_OF_MEMORY};
    pool->workers = (worker_slice){aligned_alloc(_Alignof(worker), count * sizeof(worker)), count};
    pool->capacity = capacity;
    if (pool->workers.ptr == NULL) { free(pool); return (worker_pool_create_result){.tag = WORKER_POOL_CREATE_OUT_OF_MEMORY}; }
    SliceFor(pool->workers, index) {
        worker *state = &pool->workers.ptr[index];
        *state = (worker){0};
        atomic_init(&state->stop, 0);
        atomic_init(&state->broken, 0);
    }
    worker_pool_create_result result = {.tag = WORKER_POOL_CREATE_OK};
    SliceFor(pool->workers, index) {
        worker *state = &pool->workers.ptr[index];
        worker_buffer_create_result buffer = WorkerBufferCreate(&state->requests, capacity, sizeof(worker_job));
        if (buffer.tag == WORKER_BUFFER_CREATE_OK)
            buffer = WorkerBufferCreate(&state->results, capacity, sizeof(worker_result));
        switch (buffer.tag) {
        case WORKER_BUFFER_CREATE_OK: break;
        case WORKER_BUFFER_CREATE_OVERFLOW: result.tag = WORKER_POOL_CREATE_OVERFLOW; goto failed;
        case WORKER_BUFFER_CREATE_OUT_OF_MEMORY: result.tag = WORKER_POOL_CREATE_OUT_OF_MEMORY; goto failed;
        case WORKER_BUFFER_CREATE_NOT_SUPPORTED: result.tag = WORKER_POOL_CREATE_NOT_SUPPORTED; goto failed;
        case WORKER_BUFFER_CREATE_INVALID_ARGUMENT: result.tag = WORKER_POOL_CREATE_INVALID_ARGUMENT; goto failed;
        }
        os_cpu_id_slice affinity = {0};
        if (placement.len != 0) affinity = (os_cpu_id_slice){&placement.ptr[index % placement.len], 1};
        thread_start_result started = ThreadStart(WorkerEntry, state, affinity, &state->thread);
        switch (started.tag) {
        case THREAD_START_OK: break;
        case THREAD_START_INVALID_ARGUMENT: result.tag = WORKER_POOL_CREATE_INVALID_ARGUMENT; goto failed;
        case THREAD_START_OUT_OF_MEMORY: result.tag = WORKER_POOL_CREATE_OUT_OF_MEMORY; goto failed;
        case THREAD_START_SYSTEM_ERROR: result.tag = WORKER_POOL_CREATE_SYSTEM_ERROR; result.native_error = started.native_error; goto failed;
        }
    }
    *output = pool;
    return result;
failed:
    {
        worker_pool_release_result released = WorkerPoolRelease(pool);
        if (released.tag != WORKER_POOL_RELEASE_OK)
            return (worker_pool_create_result){.tag = WORKER_POOL_CREATE_SYSTEM_ERROR, .native_error = released.native_error};
    }
    return result;
}

usize WorkerPoolCount(const worker_pool *pool) { return pool == NULL ? 0 : pool->workers.len; }

worker_select_result WorkerSelect(worker_pool *pool, usize start)
{
    if (!pool || start >= pool->workers.len) return (worker_select_result){.tag = WORKER_SELECT_INVALID_ARGUMENT};
    usize best = start;
    SliceFor(pool->workers, offset) {
        usize index = (start + offset) % pool->workers.len;
        if (pool->workers.ptr[index].pending < pool->workers.ptr[best].pending) best = index;
    }
    return (worker_select_result){.tag = WORKER_SELECT_OK, .index = best};
}

worker_submit_result WorkerSubmit(worker_pool *pool, usize index, worker_job job)
{
    if (pool == NULL || index >= pool->workers.len || job.function == NULL ||
        (job.input.ptr == NULL && job.input.len != 0)) return (worker_submit_result){.tag = WORKER_SUBMIT_INVALID_ARGUMENT};
    worker *state = &pool->workers.ptr[index];
    if (atomic_load_explicit(&state->broken, memory_order_acquire)) return (worker_submit_result){.tag = WORKER_SUBMIT_BROKEN};
    if (state->pending == pool->capacity) return (worker_submit_result){.tag = WORKER_SUBMIT_WOULD_BLOCK};
    spsc_write_result written = SpscWrite(&state->requests, BytesOf(job));
    if (written.tag != SPSC_WRITE_OK || written.value != sizeof(job)) return (worker_submit_result){.tag = WORKER_SUBMIT_BROKEN};
    ++state->pending;
    return (worker_submit_result){.tag = WORKER_SUBMIT_OK};
}

worker_receive_result WorkerReceive(worker_pool *pool, usize index, worker_result *result)
{
    if (pool == NULL || index >= pool->workers.len || result == NULL) return (worker_receive_result){.tag = WORKER_RECEIVE_INVALID_ARGUMENT};
    worker *state = &pool->workers.ptr[index];
    if (atomic_load_explicit(&state->broken, memory_order_acquire)) return (worker_receive_result){.tag = WORKER_RECEIVE_BROKEN};
    spsc_read_result read = SpscRead(&state->results, (byte_slice){(u8 *)result, sizeof(*result)});
    if (read.tag != SPSC_READ_OK) return (worker_receive_result){.tag = WORKER_RECEIVE_BROKEN};
    if (read.value == 0) return (worker_receive_result){.tag = WORKER_RECEIVE_EMPTY};
    if (read.value != sizeof(*result) || state->pending == 0) return (worker_receive_result){.tag = WORKER_RECEIVE_BROKEN};
    --state->pending;
    return (worker_receive_result){.tag = WORKER_RECEIVE_OK};
}

worker_pool_destroy_result WorkerPoolDestroy(worker_pool *pool)
{
    if (pool == NULL) return (worker_pool_destroy_result){.tag = WORKER_POOL_DESTROY_OK};
    SliceFor(pool->workers, index) {
        if (pool->workers.ptr[index].pending != 0) return (worker_pool_destroy_result){.tag = WORKER_POOL_DESTROY_BUSY};
    }
    worker_pool_release_result released = WorkerPoolRelease(pool);
    return (worker_pool_destroy_result){.tag = released.tag == WORKER_POOL_RELEASE_OK ?
        WORKER_POOL_DESTROY_OK : WORKER_POOL_DESTROY_SYSTEM_ERROR, .native_error = released.native_error};
}
