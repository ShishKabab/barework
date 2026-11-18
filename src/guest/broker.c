#include "app-broker.h"
#include "broker_protocol.h"

static bump_allocator arena;
static byte_slice request;
static app_route_payload route;
static u8 input_storage[QUERY_MESSAGE_LIMIT];
static const_byte_slice input;
static business_init_result initialized;
static business_handle_result handled;
static query_arguments arguments;
static query_result failure;
static query_call pending;
static u32 event, completed;
static bool started, ready, admitted, waiting, failed, done;

u32 BrokerInit(u32 version, u32 length)
{
    initialized.tag = BUSINESS_INIT_INVALID_ARGUMENT;
    if (started || version != BUSINESS_ABI_VERSION || length < sizeof(route_request) || length > CONNECTION_DISPATCH_LIMIT)
        return (u32)(uintptr_t)&initialized;
    started = true;
    if (BumpCreate(&arena, CONNECTION_DISPATCH_LIMIT).tag != BUMP_CREATE_OK) {
        initialized.tag = BUSINESS_INIT_NO_SPACE;
        return (u32)(uintptr_t)&initialized;
    }
    bump_allocate_result allocation = BumpAllocate(&arena, length, _Alignof(route_request));
    if (allocation.tag != BUMP_ALLOCATE_OK) {
        initialized.tag = BUSINESS_INIT_NO_SPACE;
        return (u32)(uintptr_t)&initialized;
    }
    request = allocation.allocation;
    ready = true;
    event = BROKER_REQUEST;
    initialized.tag = BUSINESS_INIT_OK;
    initialized.input = (wasm_slice){(u32)(uintptr_t)request.ptr, (u32)request.len};
    return (u32)(uintptr_t)&initialized;
}
u32 BrokerContinue(u32 type, u32 length)
{
    initialized.tag = BUSINESS_INIT_INVALID_ARGUMENT;
    if (!admitted || ready || done || length > sizeof(input_storage) ||
        (type != BROKER_QUERY_REQUEST && type != BROKER_QUERY_RESULT && type != BROKER_FINISH)) return (u32)(uintptr_t)&initialized;
    input = (const_byte_slice){input_storage, length};
    event = type;
    ready = true;
    initialized.tag = BUSINESS_INIT_OK;
    initialized.input = (wasm_slice){(u32)(uintptr_t)input_storage, length};
    return (u32)(uintptr_t)&initialized;
}
static void Output(u32 type, const_byte_slice bytes)
{
    handled = (business_handle_result){.tag = BUSINESS_HANDLE_OK, .type = type,
        .output = {(u32)(uintptr_t)bytes.ptr, (u32)bytes.len}};
}
u32 BrokerHandle(void)
{
    handled = (business_handle_result){.tag = BUSINESS_HANDLE_INVALID_STATE};
    if (!ready || done) return (u32)(uintptr_t)&handled;
    ready = false;
    const_byte_slice immutable = {request.ptr, request.len};
    if (event == BROKER_REQUEST) {
        if (admitted || RouteRequestRead(immutable, &route).tag != ROUTE_REQUEST_READ_OK ||
            AppBrokerExpected(route.tag, 0).tag == APP_BROKER_EXPECTED_INVALID) goto denied;
        /* Admission is deliberately limited to synthetic anonymous reads. */
        admitted = true;
        Output(BROKER_ADMITTED, (const_byte_slice){0});
    } else if (event == BROKER_QUERY_REQUEST) {
        query_call call;
        app_broker_expected_result expected = AppBrokerExpected(route.tag, completed);
        if (waiting || failed || expected.tag != APP_BROKER_EXPECTED_QUERY || input.len != sizeof(call) ||
            ByteSliceCopy(MutableBytesOf(call), 0, input).tag != BYTE_SLICE_COPY_OK ||
            call.operation != expected.operation || call.sequence != completed + 1) goto denied;
        app_broker_arguments_result bound = AppBrokerArguments(immutable, &route, call);
        if (bound.tag == APP_BROKER_ARGUMENTS_DENIED) goto denied;
        if (bound.tag == APP_BROKER_ARGUMENTS_INVALID_PARAMETER) {
            failure = (query_result){.operation = call.operation, .sequence = call.sequence, .outcome = QUERY_INVALID_PARAMETER};
            failed = true;
            ++completed;
            Output(BROKER_QUERY_RESULT, BytesOf(failure));
        } else {
            arguments = bound.arguments;
            pending = call;
            waiting = true;
            Output(BROKER_EXECUTE_QUERY, BytesOf(arguments));
        }
    } else if (event == BROKER_QUERY_RESULT) {
        app_query_shape_result shape = AppQueryShape(pending.operation);
        if (!waiting || shape.tag != APP_QUERY_SHAPE_OK) goto denied;
        query_validate_result result = QueryValidate(input, pending.operation, pending.sequence, shape.shape);
        if (result.tag != QUERY_VALIDATE_OK) goto denied;
        waiting = false;
        failed = result.root.outcome != QUERY_SUCCESS;
        ++completed;
        Output(BROKER_QUERY_RESULT, input);
    } else if (event == BROKER_FINISH) {
        if (waiting || input.len || (!failed && AppBrokerExpected(route.tag, completed).tag != APP_BROKER_EXPECTED_DONE)) goto denied;
        done = true;
        Output(BROKER_FINISHED, (const_byte_slice){0});
    } else goto denied;
    return (u32)(uintptr_t)&handled;
denied:
    done = true;
    Output(BROKER_REJECTED, (const_byte_slice){0});
    return (u32)(uintptr_t)&handled;
}
