#include "workflow.h"
#include "app-routes.h"
#include "app-queries.h"
#include "business_guest_bytes.h"
#include "broker_guest_bytes.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>

static business_runtime *business, *broker;
typedef struct request_fixture { http_request root; char method[3]; char target[64]; } request_fixture;
static const_byte_slice Request(string_slice path, byte_slice storage)
{
    request_fixture input = {
        .root = {.MessageLength = sizeof(input), .MethodOffset = offsetof(request_fixture, method),
            .MethodLength = 3, .TargetOffset = offsetof(request_fixture, target),
            .TargetLength = (u32)path.len, .VersionMajor = 1, .VersionMinor = 1}, .method = {'G', 'E', 'T'}};
    assert(ByteSliceCopy((byte_slice){(u8 *)input.target, sizeof(input.target)}, 0,
        (const_byte_slice){(const u8 *)path.ptr, path.len}).tag == BYTE_SLICE_COPY_OK);
    bump_allocator arena;
    assert(BumpInit(&arena, storage).tag == BUMP_INIT_OK);
    route_match_result routed = RouteMatch(BytesOf(input), &arena);
    assert(routed.tag == ROUTE_MATCH_OK);
    return routed.message;
}
static void CheckResponse(const_byte_slice response, u32 status, string_slice contains)
{
    http_response_validate_result valid = HttpResponseValidate(response);
    assert(valid.tag == HTTP_RESPONSE_VALIDATE_OK);
    http_response root;
    assert(ByteSliceCopyRange(MutableBytesOf(root), 0, response, 0, sizeof(root)).tag == BYTE_SLICE_COPY_RANGE_OK);
    assert(root.StatusCode == status);
    const_byte_range_resolve_result body = ConstByteRangeResolve(response, root.Body);
    assert(body.tag == CONST_BYTE_RANGE_RESOLVE_OK);
    bool found = !contains.len;
    SliceFor(body.bytes, index) {
        if (contains.len > body.bytes.len - index) break;
        bool same = true;
        SliceFor(contains, c) { if (body.bytes.ptr[index + c] != (u8)contains.ptr[c]) same = false; }
        if (same) found = true;
    }
    assert(found);
}
static void Run(string_slice path, u32 status, string_slice body, u32 phases)
{
    u8 request_storage[2048], response_storage[BUSINESS_RESPONSE_LIMIT];
    const_byte_slice request = Request(path, (byte_slice){request_storage, sizeof(request_storage)});
    workflow_ticket ticket = {.connection = 17, .request = 1};
    workflow_open_result opened = WorkflowOpen(business, broker, request, ticket);
    assert(opened.tag == WORKFLOW_OPEN_OK);
    workflow_step_result result;
    u32 count = 0;
    do {
        result = WorkflowStep(opened.instance, ticket, (byte_slice){response_storage, sizeof(response_storage)});
        ++count;
        if (result.tag == WORKFLOW_STEP_PENDING) {
            assert(WorkflowStep(opened.instance, ticket, (byte_slice){response_storage, sizeof(response_storage)}).tag == WORKFLOW_STEP_STALE);
            ticket = result.next;
        }
    } while (result.tag == WORKFLOW_STEP_PENDING);
    assert(result.tag == WORKFLOW_STEP_COMPLETE && count == phases);
    CheckResponse(result.response, status, body);
    assert(WorkflowClose(opened.instance).tag == WORKFLOW_CLOSE_OK);
}
static business_session *Admit(const_byte_slice request, byte_slice output)
{
    business_session_open_result opened = BusinessSessionOpen(broker, request);
    assert(opened.tag == BUSINESS_SESSION_OPEN_OK);
    business_session_step_result admitted = BusinessSessionStep(opened.session, BROKER_REQUEST, (const_byte_slice){0}, output);
    assert(admitted.tag == BUSINESS_SESSION_STEP_OK && admitted.type == BROKER_ADMITTED);
    return opened.session;
}
static void Rejects(void)
{
    u8 request_storage[2048], output_storage[QUERY_MESSAGE_LIMIT];
    const_byte_slice request = Request(StringSlice("/profile/7"), (byte_slice){request_storage, sizeof(request_storage)});
    byte_slice output = {output_storage, sizeof(output_storage)};
    query_call calls[] = {
        {BROKER_OPERATION_TIMELINE_GET_TIMELINE, 1},
        {BROKER_OPERATION_PROFILE_GET_PROFILE, 2},
        {UINT32_MAX, 1},
    };
    typedef struct calls_slice { query_call *ptr; usize len; } calls_slice;
    calls_slice cases = {calls, sizeof(calls) / sizeof(calls[0])};
    SliceFor(cases, index) {
        business_session *session = Admit(request, output);
        business_session_step_result rejected = BusinessSessionStep(session, BROKER_QUERY_REQUEST, BytesOf(cases.ptr[index]), output);
        assert(rejected.tag == BUSINESS_SESSION_STEP_OK && rejected.type == BROKER_REJECTED);
        assert(BusinessSessionClose(session, false).tag == BUSINESS_SESSION_CLOSE_OK);
    }
    business_session *session = Admit(request, output);
    business_session_step_result early = BusinessSessionStep(session, BROKER_FINISH, (const_byte_slice){0}, output);
    assert(early.tag == BUSINESS_SESSION_STEP_OK && early.type == BROKER_REJECTED);
    assert(BusinessSessionClose(session, false).tag == BUSINESS_SESSION_CLOSE_OK);
    session = Admit(request, output);
    query_call call = {BROKER_OPERATION_PROFILE_GET_PROFILE, 1};
    business_session_step_result execute = BusinessSessionStep(session, BROKER_QUERY_REQUEST, BytesOf(call), output);
    assert(execute.tag == BUSINESS_SESSION_STEP_OK && execute.type == BROKER_EXECUTE_QUERY);
    query_arguments arguments;
    assert(ByteSliceCopy(MutableBytesOf(arguments), 0, execute.output).tag == BYTE_SLICE_COPY_OK);
    assert(arguments.id == 7 && arguments.sequence == 1);
    query_result malformed = {.operation = call.operation, .sequence = 1, .rows = {.bytes = {UINT32_MAX, 16}, .count = 1}};
    business_session_step_result rejected = BusinessSessionStep(session, BROKER_QUERY_RESULT, BytesOf(malformed), output);
    assert(rejected.tag == BUSINESS_SESSION_STEP_OK && rejected.type == BROKER_REJECTED);
    assert(BusinessSessionClose(session, false).tag == BUSINESS_SESSION_CLOSE_OK);
    session = Admit(request, output);
    execute = BusinessSessionStep(session, BROKER_QUERY_REQUEST, BytesOf(call), output);
    assert(execute.tag == BUSINESS_SESSION_STEP_OK && execute.type == BROKER_EXECUTE_QUERY);
    rejected = BusinessSessionStep(session, BROKER_QUERY_REQUEST, BytesOf(call), output);
    assert(rejected.tag == BUSINESS_SESSION_STEP_OK && rejected.type == BROKER_REJECTED);
    assert(BusinessSessionClose(session, false).tag == BUSINESS_SESSION_CLOSE_OK);
}
typedef struct wrong_thread_context { workflow *instance; workflow_ticket ticket; } wrong_thread_context;
static void *WrongThread(void *argument)
{
    wrong_thread_context *context = argument;
    u8 output[BUSINESS_RESPONSE_LIMIT];
    assert(WorkflowStep(context->instance, context->ticket, (byte_slice){output, sizeof(output)}).tag == WORKFLOW_STEP_WRONG_THREAD);
    assert(WorkflowClose(context->instance).tag == WORKFLOW_CLOSE_WRONG_THREAD);
    return NULL;
}
static void Interleaved(void)
{
    u8 a[2048], b[2048], output[BUSINESS_RESPONSE_LIMIT];
    const_byte_slice first = Request(StringSlice("/profile/1"), (byte_slice){a, sizeof(a)});
    const_byte_slice second = Request(StringSlice("/profile/2"), (byte_slice){b, sizeof(b)});
    workflow_ticket tickets[2] = {{.connection = 1, .request = 1}, {.connection = 2, .request = 1}};
    workflow_open_result one = WorkflowOpen(business, broker, first, tickets[0]);
    workflow_open_result two = WorkflowOpen(business, broker, second, tickets[1]);
    assert(one.tag == WORKFLOW_OPEN_OK && two.tag == WORKFLOW_OPEN_OK);
    workflow *instances[2] = {one.instance, two.instance};
    for (usize round = 0; round < 7; ++round) {
        for (usize index = 0; index < 2; ++index) {
            workflow_step_result result = WorkflowStep(instances[index], tickets[index], (byte_slice){output, sizeof(output)});
            if (round < 6) { assert(result.tag == WORKFLOW_STEP_PENDING); tickets[index] = result.next; }
            else { assert(result.tag == WORKFLOW_STEP_COMPLETE); CheckResponse(result.response, 200, index ? StringSlice("null") : StringSlice("Synthetic example")); }
        }
    }
    assert(WorkflowClose(one.instance).tag == WORKFLOW_CLOSE_OK);
    assert(WorkflowClose(two.instance).tag == WORKFLOW_CLOSE_OK);
    one = WorkflowOpen(business, broker, first, (workflow_ticket){.connection = 1, .request = 2});
    assert(one.tag == WORKFLOW_OPEN_OK);
    workflow_step_result pending = WorkflowStep(one.instance, (workflow_ticket){.connection = 1, .request = 2}, (byte_slice){output, sizeof(output)});
    assert(pending.tag == WORKFLOW_STEP_PENDING);
    wrong_thread_context context = {one.instance, pending.next};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, WrongThread, &context) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(WorkflowClose(one.instance).tag == WORKFLOW_CLOSE_OK);
}
int main(void)
{
    assert(BusinessRuntimeCreate((const_byte_slice){business_guest_bytes, business_guest_bytes_len}, &business).tag == BUSINESS_RUNTIME_CREATE_OK);
    assert(BrokerRuntimeCreate((const_byte_slice){broker_guest_bytes, broker_guest_bytes_len}, &broker).tag == BROKER_RUNTIME_CREATE_OK);
    assert(BusinessRuntimeCanReset(business));
    Run(StringSlice("/"), 200, StringSlice("Hello, world!"), 3);
    Run(StringSlice("/profile/1"), 200, StringSlice("Synthetic example"), 7);
    Run(StringSlice("/profile/0"), 200, StringSlice("[]"), 7);
    Run(StringSlice("/profile/2"), 200, StringSlice("null"), 7);
    Run(StringSlice("/profile/13"), 503, StringSlice("query unavailable"), 7);
    Run(StringSlice("/profile/nope"), 400, StringSlice("invalid route parameter"), 5);
    Run(StringSlice("/timeline/1"), 200, StringSlice("timelineEvent"), 7);
    Run(StringSlice("/profile-summary/1"), 200, StringSlice("bio"), 11);
    Run(StringSlice("/profile-summary/13"), 503, StringSlice("query unavailable"), 7);
    Rejects();
    Interleaved();
    Run(StringSlice("/profile/1"), 200, StringSlice("Synthetic example"), 7);
    BusinessRuntimeDestroy(broker);
    BusinessRuntimeDestroy(business);
    puts("PASS: broker policy, queued queries, stale tickets, interleaved instances, cancellation, retained two-query state");
}
