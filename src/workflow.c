#include "workflow.h"
#include "app-queries.h"
#include "os.h"
#include <stdlib.h>

typedef enum workflow_phase { WORKFLOW_ADMIT, WORKFLOW_BUSINESS, WORKFLOW_AUTHORIZE,
    WORKFLOW_QUERY, WORKFLOW_VALIDATE, WORKFLOW_FINISH, WORKFLOW_DONE, WORKFLOW_FAILED } workflow_phase;
struct workflow {
    business_runtime *runtime;
    business_session *business, *broker;
    u64 owner;
    workflow_ticket ticket;
    workflow_phase phase;
    const_byte_slice request, result;
    u32 event;
    query_call call;
    query_arguments arguments;
    u8 query[QUERY_MESSAGE_LIMIT], exchange[QUERY_MESSAGE_LIMIT];
    u8 response[BUSINESS_RESPONSE_LIMIT];
    usize response_length;
};

workflow_open_result WorkflowOpen(business_runtime *business, business_runtime *broker,
    const_byte_slice request, workflow_ticket ticket)
{
    if (!business || !broker || !ticket.request || ticket.step || !request.ptr || !request.len)
        return (workflow_open_result){.tag = WORKFLOW_OPEN_INVALID};
    workflow *instance = calloc(1, sizeof(*instance));
    if (!instance) return (workflow_open_result){.tag = WORKFLOW_OPEN_NO_SPACE};
    business_session_open_result opened = BusinessSessionOpen(broker, request);
    if (opened.tag != BUSINESS_SESSION_OPEN_OK) {
        free(instance);
        return (workflow_open_result){.tag = WORKFLOW_OPEN_INVALID};
    }
    instance->runtime = business;
    instance->broker = opened.session;
    instance->owner = ThreadCurrentId();
    instance->ticket = ticket;
    instance->request = request;
    instance->event = BUSINESS_REQUEST;
    return (workflow_open_result){.tag = WORKFLOW_OPEN_OK, .instance = instance};
}

workflow_step_result WorkflowStep(workflow *instance, workflow_ticket ticket, byte_slice output)
{
    if (!instance) return (workflow_step_result){.tag = WORKFLOW_STEP_STALE};
    if (instance->owner != ThreadCurrentId()) return (workflow_step_result){.tag = WORKFLOW_STEP_WRONG_THREAD};
    if (ticket.connection != instance->ticket.connection || ticket.request != instance->ticket.request ||
        ticket.step != instance->ticket.step || instance->phase >= WORKFLOW_DONE)
        return (workflow_step_result){.tag = WORKFLOW_STEP_STALE};
    if (++instance->ticket.step > 64) goto Failed;
    byte_slice exchange = {instance->exchange, sizeof(instance->exchange)};
    business_session_step_result step;
    switch (instance->phase) {
    case WORKFLOW_ADMIT: {
        step = BusinessSessionStep(instance->broker, BROKER_REQUEST, (const_byte_slice){0}, exchange);
        if (step.tag != BUSINESS_SESSION_STEP_OK || step.type != BROKER_ADMITTED) goto Failed;
        business_session_open_result opened = BusinessSessionOpen(instance->runtime, instance->request);
        if (opened.tag != BUSINESS_SESSION_OPEN_OK) goto Failed;
        instance->business = opened.session;
        instance->phase = WORKFLOW_BUSINESS;
        break;
    }
    case WORKFLOW_BUSINESS:
        step = BusinessSessionStep(instance->business, instance->event, instance->result, exchange);
        if (step.tag != BUSINESS_SESSION_STEP_OK) goto Failed;
        if (step.type == BUSINESS_QUERY_REQUEST) {
            if (step.output.len != sizeof(instance->call) ||
                ByteSliceCopy(MutableBytesOf(instance->call), 0, step.output).tag != BYTE_SLICE_COPY_OK) goto Failed;
            instance->phase = WORKFLOW_AUTHORIZE;
        } else if (step.type == BUSINESS_RESULT) {
            if (ByteSliceCopy((byte_slice){instance->response, sizeof(instance->response)}, 0, step.output).tag != BYTE_SLICE_COPY_OK) goto Failed;
            instance->response_length = step.output.len;
            instance->phase = WORKFLOW_FINISH;
        } else goto Failed;
        break;
    case WORKFLOW_AUTHORIZE:
        step = BusinessSessionStep(instance->broker, BROKER_QUERY_REQUEST, BytesOf(instance->call), exchange);
        if (step.tag != BUSINESS_SESSION_STEP_OK) goto Failed;
        if (step.type == BROKER_EXECUTE_QUERY) {
            if (step.output.len != sizeof(instance->arguments) ||
                ByteSliceCopy(MutableBytesOf(instance->arguments), 0, step.output).tag != BYTE_SLICE_COPY_OK) goto Failed;
            instance->phase = WORKFLOW_QUERY;
        } else if (step.type == BROKER_QUERY_RESULT) {
            instance->result = step.output;
            instance->event = BROKER_QUERY_RESULT;
            instance->phase = WORKFLOW_BUSINESS;
        } else goto Failed;
        break;
    case WORKFLOW_QUERY: {
        app_query_shape_result shape = AppQueryShape(instance->arguments.operation);
        if (shape.tag != APP_QUERY_SHAPE_OK) goto Failed;
        query_fake_result queried = QueryFake(instance->arguments, shape.shape,
            (byte_slice){instance->query, sizeof(instance->query)});
        if (queried.tag != QUERY_FAKE_OK) goto Failed;
        instance->result = queried.message;
        instance->phase = WORKFLOW_VALIDATE;
        break;
    }
    case WORKFLOW_VALIDATE:
        step = BusinessSessionStep(instance->broker, BROKER_QUERY_RESULT, instance->result, exchange);
        if (step.tag != BUSINESS_SESSION_STEP_OK || step.type != BROKER_QUERY_RESULT) goto Failed;
        instance->result = step.output;
        instance->event = BROKER_QUERY_RESULT;
        instance->phase = WORKFLOW_BUSINESS;
        break;
    case WORKFLOW_FINISH:
        step = BusinessSessionStep(instance->broker, BROKER_FINISH, (const_byte_slice){0}, exchange);
        if (step.tag != BUSINESS_SESSION_STEP_OK || step.type != BROKER_FINISHED ||
            ByteSliceCopy(output, 0, (const_byte_slice){instance->response, instance->response_length}).tag != BYTE_SLICE_COPY_OK) goto Failed;
        instance->phase = WORKFLOW_DONE;
        return (workflow_step_result){.tag = WORKFLOW_STEP_COMPLETE,
            .response = {output.ptr, instance->response_length}};
    case WORKFLOW_DONE: case WORKFLOW_FAILED: goto Failed;
    }
    return (workflow_step_result){.tag = WORKFLOW_STEP_PENDING, .next = instance->ticket};
Failed:
    instance->phase = WORKFLOW_FAILED;
    return (workflow_step_result){.tag = WORKFLOW_STEP_FAILED};
}

workflow_close_result WorkflowClose(workflow *instance)
{
    if (!instance) return (workflow_close_result){.tag = WORKFLOW_CLOSE_OK};
    if (instance->owner != ThreadCurrentId()) return (workflow_close_result){.tag = WORKFLOW_CLOSE_WRONG_THREAD};
    if (BusinessSessionClose(instance->business, instance->phase == WORKFLOW_DONE).tag != BUSINESS_SESSION_CLOSE_OK ||
        BusinessSessionClose(instance->broker, false).tag != BUSINESS_SESSION_CLOSE_OK)
        return (workflow_close_result){.tag = WORKFLOW_CLOSE_WRONG_THREAD};
    free(instance);
    return (workflow_close_result){.tag = WORKFLOW_CLOSE_OK};
}
