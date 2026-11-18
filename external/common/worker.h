#ifndef COMMON_WORKER_H
#define COMMON_WORKER_H
#include "worker_results.h"

#include "os.h"

typedef struct worker_job worker_job;
typedef struct worker_result {
    u64 id;
    u32 type;
    const_byte_slice outcome; /* Job-specific tagged result, with handler-defined lifetime. */
    u32 flags; /* Job-specific result metadata. */
    usize consumed;
    const_byte_slice data;
    const_byte_slice message;
} worker_result;
typedef worker_result (*worker_function)(worker_job job);

struct worker_job {
    u64 id;
    worker_function function;
    void *context;
    const_byte_slice input;
    u32 type;
};

typedef struct worker_pool worker_pool;
/* Generic callbacks run on one selected worker, with no work stealing.
 * Exactly one controller submits/receives. Each worker owns two SPSC byte
 * buffers and continuously polls for jobs: no blocking, pause, or scheduler yield.
 * The controller must poll results too. Capacity bounds submitted-but-not-received
 * jobs per worker, reserving result space so workers never wait for the controller.
 * Input/context must remain stable until the matching result is received. Context
 * may be worker-owned mutable state; the controller must not touch it meanwhile.
 * Result data has a lifetime defined by its job handler (never an implicit copy).
 * Callback must finish without waiting for another job in this pool. */
/* Nonempty placement pins worker i to placement[i % placement.len] before it
 * starts. Extra workers share these CPUs. Empty placement inherits affinity.
 * Placement storage is borrowed only during creation. */
worker_pool_create_result WorkerPoolCreate(usize count, usize capacity, os_cpu_id_slice placement, worker_pool **pool);
usize WorkerPoolCount(const worker_pool *pool);
typedef enum worker_select_tag { WORKER_SELECT_OK, WORKER_SELECT_INVALID_ARGUMENT } worker_select_tag;
typedef struct worker_select_result { worker_select_tag tag; usize index; } worker_select_result;
/* Controller-only snapshot of submitted-but-not-received work. No additional
 * shared atomics. Rotate start to break ties fairly; selection reserves no slot. */
worker_select_result WorkerSelect(worker_pool *pool, usize start);
worker_submit_result WorkerSubmit(worker_pool *pool, usize worker, worker_job job);
/* WORKER_RECEIVE_OK supplies a result; EMPTY leaves output unchanged.
 * Other tags describe invalid arguments or a broken worker/queue. */
worker_receive_result WorkerReceive(worker_pool *pool, usize worker, worker_result *result);
/* Returns WORKER_POOL_DESTROY_BUSY until all submitted results are received. Stops and joins
 * all workers before releasing their storage. */
worker_pool_destroy_result WorkerPoolDestroy(worker_pool *pool);

#endif
