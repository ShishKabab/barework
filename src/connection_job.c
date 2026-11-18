#include "connection_job.h"
#include "slice.h"

worker_result BusinessWorkerJob(worker_job job)
{
    business_job_context *context = job.context;
    static const business_run_result invalid = {.tag = BUSINESS_RUN_INVALID_ARGUMENT};
    if (!context || job.type != BUSINESS_REQUEST)
        return (worker_result){.id = job.id, .type = BUSINESS_RESULT, .outcome = BytesOf(invalid)};
    if (context->broker) {
        if (!context->active) {
            if (context->serial == UINT64_MAX) goto Failed;
            context->ticket = (workflow_ticket){.connection = job.id, .request = ++context->serial};
            workflow_open_result opened = WorkflowOpen(context->runtime, context->broker, job.input, context->ticket);
            if (opened.tag != WORKFLOW_OPEN_OK) goto Failed;
            context->active = opened.instance;
        }
        context->progress = WorkflowStep(context->active, context->ticket,
            (byte_slice){context->output, sizeof(context->output)});
        if (context->progress.tag == WORKFLOW_STEP_PENDING) {
            context->ticket = context->progress.next;
        } else {
            if (WorkflowClose(context->active).tag != WORKFLOW_CLOSE_OK) goto Failed;
            context->active = NULL;
        }
        return (worker_result){.id = job.id, .type = BUSINESS_RESULT, .outcome = BytesOf(context->progress)};
Failed:
        context->progress = (workflow_step_result){.tag = WORKFLOW_STEP_FAILED};
        return (worker_result){.id = job.id, .type = BUSINESS_RESULT, .outcome = BytesOf(context->progress)};
    }
    context->result = BusinessRun(context->runtime, job.input,
        (byte_slice){context->output, sizeof(context->output)});
    return (worker_result){.id = job.id, .type = BUSINESS_RESULT, .outcome = BytesOf(context->result)};
}

connection_job_result ConnectionJob(worker_job job)
{
    connection_job_context *context = job.context;
    if (context == NULL || context->id != job.id)
        return (connection_job_result){.tag = CONNECTION_JOB_STALE_ID};
    connection_job_result result = {.tag = CONNECTION_JOB_OK};
    if (job.type == CONNECTION_CLOSE) {
        connection_vm_close_result closed = ConnectionVmClose(context->vm);
        switch (closed.tag) {
        case CONNECTION_VM_CLOSE_OK:
            context->vm = NULL;
            result.delivery.type = CONNECTION_CLOSED;
            return result;
        case CONNECTION_VM_CLOSE_WRONG_THREAD: return (connection_job_result){.tag = CONNECTION_JOB_WRONG_THREAD};
        case CONNECTION_VM_CLOSE_INVALID_STATE: return (connection_job_result){.tag = CONNECTION_JOB_INVALID_STATE};
        case CONNECTION_VM_CLOSE_SYSTEM_ERROR: return (connection_job_result){.tag = CONNECTION_JOB_SYSTEM_ERROR, .native_error = closed.native_error};
        }
    }
    if (job.type == CONNECTION_OPEN) {
        if (context->vm != NULL || job.input.len != 0)
            return (connection_job_result){.tag = CONNECTION_JOB_INVALID_ARGUMENT};
        connection_vm_open_result opened = ConnectionVmOpen(context->runtime, &context->vm, &result.delivery);
        switch (opened.tag) {
        case CONNECTION_VM_OPEN_OK: break;
        case CONNECTION_VM_OPEN_INVALID_ARGUMENT: result.tag = CONNECTION_JOB_INVALID_ARGUMENT; break;
        case CONNECTION_VM_OPEN_OUT_OF_MEMORY: result.tag = CONNECTION_JOB_OUT_OF_MEMORY; break;
        case CONNECTION_VM_OPEN_INVALID_GUEST: result.tag = CONNECTION_JOB_INVALID_GUEST; break;
        case CONNECTION_VM_OPEN_SYSTEM_ERROR: result.tag = CONNECTION_JOB_SYSTEM_ERROR; result.native_error = opened.native_error; break;
        }
    } else {
        connection_vm_step_result stepped = ConnectionVmStep(context->vm, job.type, job.input, &result.delivery);
        if (stepped.tag == CONNECTION_VM_STEP_OK && result.delivery.flags == CONNECTION_HAS_REQUEST && !context->dispatch_via_main && !context->business.broker) {
            connection_delivery parsed = result.delivery;
            business_run_result business = BusinessRun(context->business.runtime, parsed.request,
                (byte_slice){context->business.output, sizeof(context->business.output)});
            stepped = ConnectionVmStep(context->vm, business.tag == BUSINESS_RUN_OK ? CONNECTION_RESPONSE : CONNECTION_BUSINESS_FAILED,
                business.tag == BUSINESS_RUN_OK ? business.response : (const_byte_slice){0}, &result.delivery);
            if (stepped.tag == CONNECTION_VM_STEP_OK) {
                result.delivery.consumed = parsed.consumed;
                result.delivery.request = parsed.request;
                result.delivery.flags |= CONNECTION_HAS_REQUEST;
            }
        }
        switch (stepped.tag) {
        case CONNECTION_VM_STEP_OK: break;
        case CONNECTION_VM_STEP_INVALID_ARGUMENT: result.tag = CONNECTION_JOB_INVALID_ARGUMENT; break;
        case CONNECTION_VM_STEP_WRONG_THREAD: result.tag = CONNECTION_JOB_WRONG_THREAD; break;
        case CONNECTION_VM_STEP_OUT_OF_MEMORY: result.tag = CONNECTION_JOB_OUT_OF_MEMORY; break;
        case CONNECTION_VM_STEP_INVALID_GUEST: result.tag = CONNECTION_JOB_INVALID_GUEST; break;
        }
    }
    return result;
}

worker_result ConnectionWorkerJob(worker_job job)
{
    connection_job_context *context = job.context;
    static const connection_job_result stale = {.tag = CONNECTION_JOB_STALE_ID};
    worker_result response = {.id = job.id, .type = CONNECTION_REQUEST, .outcome = BytesOf(stale)};
    if (context == NULL || context->id != job.id) return response;
    context->last_result = ConnectionJob(job);
    response.outcome = BytesOf(context->last_result);
    if (context->last_result.tag == CONNECTION_JOB_OK) {
        connection_delivery delivery = context->last_result.delivery;
        response.type = delivery.type;
        response.flags = delivery.flags;
        response.consumed = delivery.consumed;
        response.data = delivery.output;
        response.message = delivery.request;
    }
    return response;
}
