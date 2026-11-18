#define _GNU_SOURCE
#include "worker.h"
#include "slice.h"

#include <assert.h>
#include <sched.h>

typedef struct counter { usize next; } counter;
typedef struct counter_slice { counter *ptr; usize len; } counter_slice;

static worker_result CountJob(worker_job job)
{
    counter *state = job.context;
    assert(job.type == state->next++);
    return (worker_result){.id = UINT64_MAX, .type = 73, .consumed = state->next, .data = job.input};
}

static worker_result Receive(worker_pool *pool, usize worker)
{
    for (;;) {
        worker_result result;
        worker_receive_result ready = WorkerReceive(pool, worker, &result);
        assert(ready.tag == WORKER_RECEIVE_OK || ready.tag == WORKER_RECEIVE_EMPTY);
        if (ready.tag == WORKER_RECEIVE_OK) return result;
    }
}

static void TestTopology(void)
{
    os_cpu_info info;
    assert(OsCpuInfo(NULL).tag == OS_CPU_INFO_INVALID_ARGUMENT);
    assert(OsCpuInfo(&info).tag == OS_CPU_INFO_OK);
    assert(info.core_count > 0 && info.core_count <= info.logical_count);
    assert(info.performance_count + info.efficiency_count + info.unknown_count == info.core_count);
    assert(info.unresolved_count <= info.core_count);
    assert(info.core_cpus.len == info.core_count);
    assert(info.performance_cpus.len == info.performance_count);
    OsCpuInfoDestroy(&info);
    /* Linux test boundary: temporarily restrict this thread to one allowed CPU.
     * Skip only when this fixed test mask cannot represent the host's CPUs. */
    cpu_set_t original;
    if (sched_getaffinity(0, sizeof(original), &original) == 0) {
        i32 first = -1;
        for (i32 cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &original)) { first = cpu; break; }
        }
        assert(first >= 0);
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(first, &one);
        assert(sched_setaffinity(0, sizeof(one), &one) == 0);
        assert(OsCpuInfo(&info).tag == OS_CPU_INFO_OK);
        assert(info.logical_count == 1 && info.core_count == 1);
        assert(info.core_cpus.len == 1 && info.core_cpus.ptr[0] == (u32)first);
        OsCpuInfoDestroy(&info);
        assert(sched_setaffinity(0, sizeof(original), &original) == 0);
    }
}

static void TestPool(void)
{
    worker_pool *pool;
    assert(WorkerPoolCreate(0, 1, (os_cpu_id_slice){0}, &pool).tag == WORKER_POOL_CREATE_INVALID_ARGUMENT);
    assert(WorkerPoolCreate(1, 0, (os_cpu_id_slice){0}, &pool).tag == WORKER_POOL_CREATE_INVALID_ARGUMENT);
    assert(WorkerPoolCreate(2, 3, (os_cpu_id_slice){0}, &pool).tag == WORKER_POOL_CREATE_OK);
    assert(WorkerPoolCount(pool) == 2);
    counter storage[2] = {0};
    counter_slice counters = {storage, 2};
    static const u8 bytes[] = {1, 2, 3, 4};
    const_byte_slice input = {bytes, sizeof(bytes)};
    /* Capacity three exercises byte-ring wrap across non-power-of-two records.
     * Do not read results until full: credits must bound unread results too. */
    for (usize batch = 0; batch < 3000; ++batch) {
        SliceFor(counters, worker) {
            for (usize index = 0; index < 3; ++index) {
                worker_job job = {.id = index + 3 * batch, .function = CountJob,
                    .context = &counters.ptr[worker], .type = (u32)(index + 3 * batch), .input = input};
                assert(WorkerSubmit(pool, worker, job).tag == WORKER_SUBMIT_OK);
            }
            worker_job extra = {.function = CountJob};
            assert(WorkerSubmit(pool, worker, extra).tag == WORKER_SUBMIT_WOULD_BLOCK);
        }
        assert(WorkerPoolDestroy(pool).tag == WORKER_POOL_DESTROY_BUSY);
        SliceFor(counters, worker) {
            for (usize index = 0; index < 3; ++index) {
                worker_result result = Receive(pool, worker);
                assert(result.id == index + 3 * batch);
                assert(result.type == 73 && result.consumed == result.id + 1);
                assert(result.data.ptr == input.ptr && result.data.len == input.len);
            }
        }
    }
    assert(WorkerPoolDestroy(pool).tag == WORKER_POOL_DESTROY_OK);
    assert(WorkerPoolDestroy(NULL).tag == WORKER_POOL_DESTROY_OK);
}

typedef struct affinity_context { u32 cpu; bool performance; } affinity_context;
typedef struct affinity_context_slice { affinity_context *ptr; usize len; } affinity_context_slice;

static void CheckAffinity(void *argument)
{
    affinity_context *context = argument;
    os_cpu_info info;
    assert(OsCpuInfo(&info).tag == OS_CPU_INFO_OK);
    assert(info.logical_count == 1 && info.core_cpus.len == 1);
    assert(info.core_cpus.ptr[0] == context->cpu);
    if (context->performance) assert(info.performance_count == 1);
    assert(sched_getcpu() == (i32)context->cpu);
    OsCpuInfoDestroy(&info);
}

static worker_result AffinityJob(worker_job job)
{
    CheckAffinity(job.context);
    return (worker_result){.id = job.id};
}

static void TestPinning(void)
{
    os_cpu_info before;
    assert(OsCpuInfo(&before).tag == OS_CPU_INFO_OK);
    os_cpu_id_slice placement = before.performance_cpus.len ? before.performance_cpus : before.core_cpus;
    /* Exercise sharing with three workers and at most two distinct cores. */
    if (placement.len > 2) placement.len = 2;
    affinity_context contexts_storage[3];
    affinity_context_slice contexts = {contexts_storage, 3};
    worker_pool *pool;
    assert(WorkerPoolCreate(3, 2, placement, &pool).tag == WORKER_POOL_CREATE_OK);
    SliceFor(contexts, index) {
        contexts.ptr[index] = (affinity_context){placement.ptr[index % placement.len], before.performance_count != 0};
        worker_job job = {.id = index, .function = AffinityJob, .context = &contexts.ptr[index]};
        assert(WorkerSubmit(pool, index, job).tag == WORKER_SUBMIT_OK);
    }
    SliceFor(contexts, index) assert(Receive(pool, index).id == index);
    assert(WorkerPoolDestroy(pool).tag == WORKER_POOL_DESTROY_OK);

    os_thread *thread;
    assert(ThreadStart(CheckAffinity, &contexts.ptr[0], (os_cpu_id_slice){placement.ptr, 1}, &thread).tag == THREAD_START_OK);
    assert(ThreadJoin(thread).tag == THREAD_JOIN_OK);
    u32 unavailable = UINT32_MAX;
    assert(ThreadStart(CheckAffinity, &contexts.ptr[0], (os_cpu_id_slice){&unavailable, 1}, &thread).tag == THREAD_START_INVALID_ARGUMENT);
    assert(thread == NULL);
    assert(ThreadStart(CheckAffinity, &contexts.ptr[0], (os_cpu_id_slice){NULL, 1}, &thread).tag == THREAD_START_INVALID_ARGUMENT);

    /* The first worker starts; the second fails. Creation must join the first
     * and release its queues without publishing a partially usable pool. */
    u32 partially_valid[] = {placement.ptr[0], UINT32_MAX};
    assert(WorkerPoolCreate(2, 2, (os_cpu_id_slice){partially_valid, 2}, &pool).tag == WORKER_POOL_CREATE_INVALID_ARGUMENT);
    assert(pool == NULL);

    cpu_set_t original;
    if (placement.len == 2 && placement.ptr[0] < CPU_SETSIZE && placement.ptr[1] < CPU_SETSIZE &&
        sched_getaffinity(0, sizeof(original), &original) == 0) {
        cpu_set_t restricted;
        CPU_ZERO(&restricted);
        CPU_SET(placement.ptr[0], &restricted);
        assert(sched_setaffinity(0, sizeof(restricted), &restricted) == 0);
        assert(ThreadStart(CheckAffinity, &contexts.ptr[1], (os_cpu_id_slice){&placement.ptr[1], 1}, &thread).tag == THREAD_START_INVALID_ARGUMENT);
        assert(thread == NULL);
        assert(sched_setaffinity(0, sizeof(original), &original) == 0);
    }
    os_cpu_info after;
    assert(OsCpuInfo(&after).tag == OS_CPU_INFO_OK);
    assert(after.logical_count == before.logical_count && after.core_cpus.len == before.core_cpus.len);
    SliceFor(before.core_cpus, index) { assert(before.core_cpus.ptr[index] == after.core_cpus.ptr[index]); }
    OsCpuInfoDestroy(&after);
    OsCpuInfoDestroy(&before);
}

int main(void)
{
    TestTopology();
    TestPool();
    TestPinning();
    TerminalWrite(StringSlice("PASS: CPU topology, worker pinning/sharing, restricted masks, partial startup cleanup, ordering/wrap/backpressure/polling\n"));
    return 0;
}
