#ifndef NEWS_CONNECTION_JOB_H
#define NEWS_CONNECTION_JOB_H
#include "connection_vm.h"
#include "worker.h"
#include "business_vm.h"
#include "workflow.h"

typedef struct business_job_context {
    business_runtime *runtime;
    business_runtime *broker;
    workflow *active;
    workflow_ticket ticket;
    workflow_step_result progress;
    u64 serial;
    business_run_result result;
    u8 output[BUSINESS_RESPONSE_LIMIT];
} business_job_context;
worker_result BusinessWorkerJob(worker_job job);

typedef enum connection_job_tag {
    CONNECTION_JOB_OK, CONNECTION_JOB_INVALID_ARGUMENT, CONNECTION_JOB_STALE_ID,
    CONNECTION_JOB_OUT_OF_MEMORY, CONNECTION_JOB_INVALID_GUEST,
    CONNECTION_JOB_WRONG_THREAD, CONNECTION_JOB_INVALID_STATE, CONNECTION_JOB_SYSTEM_ERROR,
} connection_job_tag;
typedef struct connection_job_result {
    connection_job_tag tag;
    i32 native_error; /* Only for SYSTEM_ERROR. */
    connection_delivery delivery; /* Only for OK. */
} connection_job_result;
typedef struct connection_job_context {
    connection_runtime *runtime;
    u64 id; /* Immutable generation-tagged ID while assigned. */
    connection_vm *vm; /* Owned exclusively by assigned worker. */
    connection_job_result last_result; /* Published through the result SPSC. */
    business_job_context business;
    bool dispatch_via_main;
} connection_job_context;
/* Execute only on the owner thread. CLOSE is required after an error too. */
connection_job_result ConnectionJob(worker_job job);
/* Generic worker adapter: outcome is a borrowed connection_job_result, valid
 * until the next job on this context. The consumer checks length and copies it
 * before inspecting its tag. No shared numeric status convention. */
worker_result ConnectionWorkerJob(worker_job job);
#endif
