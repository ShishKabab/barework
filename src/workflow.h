#ifndef NEWS_WORKFLOW_H
#define NEWS_WORKFLOW_H
#include "business_vm.h"
#include "broker_protocol.h"

typedef struct workflow workflow;
typedef struct workflow_ticket { u64 connection, request; u32 step; } workflow_ticket;
typedef enum workflow_open_tag { WORKFLOW_OPEN_OK, WORKFLOW_OPEN_INVALID, WORKFLOW_OPEN_NO_SPACE } workflow_open_tag;
typedef struct workflow_open_result { workflow_open_tag tag; workflow *instance; } workflow_open_result;
workflow_open_result WorkflowOpen(business_runtime *business, business_runtime *broker,
    const_byte_slice request, workflow_ticket ticket);
typedef enum workflow_step_tag { WORKFLOW_STEP_PENDING, WORKFLOW_STEP_COMPLETE, WORKFLOW_STEP_FAILED, WORKFLOW_STEP_STALE, WORKFLOW_STEP_WRONG_THREAD } workflow_step_tag;
typedef struct workflow_step_result {
    workflow_step_tag tag;
    workflow_ticket next;
    const_byte_slice response;
} workflow_step_result;
/* One bounded phase per invocation. Tickets are host-owned, never guest input.
 * Request bytes remain stable until Close. Response borrows caller output. */
workflow_step_result WorkflowStep(workflow *instance, workflow_ticket ticket, byte_slice output);
typedef enum workflow_close_tag { WORKFLOW_CLOSE_OK, WORKFLOW_CLOSE_WRONG_THREAD } workflow_close_tag;
typedef struct workflow_close_result { workflow_close_tag tag; } workflow_close_result;
/* Owner-thread cancellation discards both guests. Successful terminal workflows
 * release the broker and reset the business instance into its thread's cache. */
workflow_close_result WorkflowClose(workflow *instance);
#endif
