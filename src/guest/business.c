#include "app-business.h"

static bump_allocator arena;
static byte_slice request, input;
static app_route_payload route;
static app_business_machine machine;
static business_init_result initialized;
static business_handle_result handled;
static u32 event;
static bool started, ready;

u32 BusinessInit(u32 version, u32 length)
{
    initialized.tag = BUSINESS_INIT_INVALID_ARGUMENT;
    if (started || version != BUSINESS_ABI_VERSION || length < sizeof(route_request) || length > CONNECTION_DISPATCH_LIMIT)
        return (u32)(uintptr_t)&initialized;
    started = true;
    if (BumpCreate(&arena, CONNECTION_DISPATCH_LIMIT + 8 * QUERY_MESSAGE_LIMIT + 4 * HTTP_RESPONSE_LIMIT).tag != BUMP_CREATE_OK) {
        initialized.tag = BUSINESS_INIT_NO_SPACE;
        return (u32)(uintptr_t)&initialized;
    }
    bump_allocate_result allocation = BumpAllocate(&arena, length, _Alignof(route_request));
    if (allocation.tag != BUMP_ALLOCATE_OK) {
        initialized.tag = BUSINESS_INIT_NO_SPACE;
        return (u32)(uintptr_t)&initialized;
    }
    request = allocation.allocation;
    event = BUSINESS_REQUEST;
    ready = true;
    initialized.tag = BUSINESS_INIT_OK;
    initialized.input = (wasm_slice){(u32)(uintptr_t)request.ptr, (u32)request.len};
    return (u32)(uintptr_t)&initialized;
}

u32 BusinessContinue(u32 type, u32 length)
{
    initialized.tag = BUSINESS_INIT_INVALID_ARGUMENT;
    if (!started || ready || !machine.waiting || machine.done || type != BROKER_QUERY_RESULT ||
        length < sizeof(query_result) || length > QUERY_MESSAGE_LIMIT) return (u32)(uintptr_t)&initialized;
    bump_allocate_result allocation = BumpAllocate(&arena, length, _Alignof(query_result));
    if (allocation.tag != BUMP_ALLOCATE_OK) {
        initialized.tag = BUSINESS_INIT_NO_SPACE;
        return (u32)(uintptr_t)&initialized;
    }
    input = allocation.allocation;
    event = type;
    ready = true;
    initialized.tag = BUSINESS_INIT_OK;
    initialized.input = (wasm_slice){(u32)(uintptr_t)input.ptr, (u32)input.len};
    return (u32)(uintptr_t)&initialized;
}

u32 BusinessHandle(void)
{
    handled = (business_handle_result){.tag = BUSINESS_HANDLE_INVALID_STATE};
    if (!ready) return (u32)(uintptr_t)&handled;
    ready = false;
    const_byte_slice message = {request.ptr, request.len};
    if (event == BUSINESS_REQUEST && RouteRequestRead(message, &route).tag != ROUTE_REQUEST_READ_OK) {
        handled.tag = BUSINESS_HANDLE_INVALID_REQUEST;
        return (u32)(uintptr_t)&handled;
    }
    app_business_step_result stepped = AppBusinessStep(&machine, &route, message, event,
        (const_byte_slice){input.ptr, input.len}, &arena);
    if (stepped.tag != APP_BUSINESS_STEP_OK) return (u32)(uintptr_t)&handled;
    handled = (business_handle_result){.tag = BUSINESS_HANDLE_OK, .type = stepped.type,
        .output = {(u32)(uintptr_t)stepped.output.ptr, (u32)stepped.output.len}};
    return (u32)(uintptr_t)&handled;
}
