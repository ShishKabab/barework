#include "connection_protocol.h"
#include "http_response.h"
#include "slice.h"
#include "business_protocol.h"
#include "app-routes.h"

static connection_control control;
static connection_init_result init_result;
static u8 input_storage[CONNECTION_INPUT_SIZE];
static u8 response_storage[BUSINESS_RESPONSE_LIMIT];
static u8 output_storage[4096];
static connection_action action_storage[CONNECTION_MAX_ACTIONS];
static http_parser parser;
static const_byte_slice pending;
static i32 initialized;
static i32 request_complete;
static i32 awaiting_send;
static i32 awaiting_business;
static i32 head;
static i32 keep_alive;
static i32 closed;

static wasm_slice GuestSlice(const_byte_slice bytes)
{
    return (wasm_slice){(u32)(uintptr_t)bytes.ptr, (u32)bytes.len};
}

static void Action(u32 type, const_byte_slice data)
{
    if (control.actions.len >= CONNECTION_MAX_ACTIONS) __builtin_trap();
    action_storage[control.actions.len++] = (connection_action){type, GuestSlice(data)};
}

static void Close(void)
{
    closed = 1;
    Action(CONNECTION_CLOSE_REQUEST, (const_byte_slice){0});
}

static void Parse(void)
{
    if (request_complete) {
        /* The worker copied the previous message before the call leading here.
         * Only this request arena rewinds; global allocation_end never does. */
        if (HttpParserReset(&parser).tag != HTTP_PARSER_RESET_READY) { Close(); return; }
        request_complete = 0;
    }
    http_parse_result result = HttpParserParse(&parser, pending);
    pending = result.remainder;
    if (result.status == HTTP_PARSE_NEED_MORE) {
        Action(CONNECTION_NEED_INPUT, (const_byte_slice){0});
        return;
    }
    const_byte_slice message = HttpParserMessage(&parser);
    http_wire_response response = HttpResponse(message, result.status);
    keep_alive = response.keep_alive;
    request_complete = 1;
    if (result.status == HTTP_PARSE_COMPLETE && response.code == 200) {
        route_match_result matched = RouteMatch(message, &parser.arena);
        switch (matched.tag) {
        case ROUTE_MATCH_OK:
            head = response.head;
            Action(CONNECTION_ROUTED_REQUEST, matched.message);
            awaiting_business = 1;
            return;
        case ROUTE_MATCH_NOT_FOUND:
            response = HttpNotFoundResponse(response.head, response.keep_alive);
            break;
        case ROUTE_MATCH_INVALID_REQUEST:
        case ROUTE_MATCH_NO_SPACE:
            response = HttpFailureResponse();
            keep_alive = 0;
            break;
        }
    }
    if (message.len != 0) Action(CONNECTION_HTTP_REQUEST, message);
    awaiting_send = 1;
    Action(CONNECTION_SEND_BYTES, (const_byte_slice){(const u8 *)response.bytes.ptr, response.bytes.len});
}

u32 ConnectionInit(u32 version)
{
    init_result = (connection_init_result){.tag = CONNECTION_INIT_OK};
    if (initialized) {
        init_result.tag = CONNECTION_INIT_ALREADY_INITIALIZED;
        return (u32)(uintptr_t)&init_result;
    }
    if (version != CONNECTION_ABI_VERSION) {
        init_result.tag = CONNECTION_INIT_UNSUPPORTED_VERSION;
        return (u32)(uintptr_t)&init_result;
    }
    initialized = 1;
    control = (connection_control){.abi_version = CONNECTION_ABI_VERSION,
        .struct_size = sizeof(control), .input = {(u32)(uintptr_t)input_storage, sizeof(input_storage)},
        .output_type = CONNECTION_REQUEST, .actions = {(u32)(uintptr_t)action_storage, 0},
        .response_input = {(u32)(uintptr_t)response_storage, sizeof(response_storage)}};
    /* Input/control/connection state are static. The request arena is the final
     * global allocation, so it may grow contiguously and retain that capacity. */
    if (HttpParserCreate(&parser, CONNECTION_MESSAGE_LIMIT + CONNECTION_DISPATCH_LIMIT, HttpDefaultLimits()).tag != HTTP_PARSER_CREATE_READY) {
        init_result.tag = CONNECTION_INIT_NO_SPACE;
        return (u32)(uintptr_t)&init_result;
    }
    Action(CONNECTION_NEED_INPUT, (const_byte_slice){0});
    init_result.control_offset = (u32)(uintptr_t)&control;
    return (u32)(uintptr_t)&init_result;
}

void ConnectionStep(void)
{
    control.actions.len = 0;
    control.output_type = CONNECTION_REQUEST;
    control.result.tag = CONNECTION_STEP_OK;
    control.consumed = 0;
    if (!initialized || closed) { control.result.tag = CONNECTION_STEP_INVALID_STATE; return; }
    switch (control.input_type) {
    case CONNECTION_RECEIVE:
        if (awaiting_business || awaiting_send || pending.len != 0 || control.input_length == 0 ||
            control.input_length > sizeof(input_storage)) break;
        pending = (const_byte_slice){input_storage, control.input_length};
        control.consumed = control.input_length;
        Parse();
        return;
    case CONNECTION_SEND_COMPLETE:
        if (!awaiting_send || control.input_length != 0) break;
        awaiting_send = 0;
        if (!keep_alive) { Close(); return; }
        if (pending.len != 0) Parse();
        else Action(CONNECTION_NEED_INPUT, (const_byte_slice){0});
        return;
    case CONNECTION_RESPONSE:
    case CONNECTION_BUSINESS_FAILED:
        if (!awaiting_business || control.input_length > sizeof(response_storage) ||
            (control.input_type == CONNECTION_BUSINESS_FAILED && control.input_length != 0)) break;
        awaiting_business = 0;
        awaiting_send = 1;
        control.consumed = control.input_length;
        if (control.input_type == CONNECTION_RESPONSE) {
            http_response_serialize_result response = HttpResponseSerialize(
                (const_byte_slice){response_storage, control.input_length}, head, keep_alive,
                (byte_slice){output_storage, sizeof(output_storage)});
            if (response.tag == HTTP_RESPONSE_SERIALIZE_OK) {
                Action(CONNECTION_SEND_BYTES, response.bytes);
                return;
            }
        }
        {
            http_wire_response failure = HttpFailureResponse();
            keep_alive = 0;
            Action(CONNECTION_SEND_BYTES, (const_byte_slice){(const u8 *)failure.bytes.ptr, failure.bytes.len});
        }
        return;
    case CONNECTION_EOF:
        if (awaiting_business || awaiting_send || pending.len != 0 || control.input_length != 0) break;
        if (request_complete || parser.wire_bytes == 0) { Close(); return; }
        {
            http_parser_finish_result finished = HttpParserFinish(&parser);
            http_parse_status status = finished.tag == HTTP_PARSER_FINISH_LIMIT ? HTTP_PARSE_LIMIT :
                finished.tag == HTTP_PARSER_FINISH_NO_SPACE ? HTTP_PARSE_NO_SPACE : HTTP_PARSE_TRUNCATED;
            http_wire_response response = HttpResponse((const_byte_slice){0}, status);
            awaiting_send = 1;
            keep_alive = 0;
            Action(CONNECTION_SEND_BYTES, (const_byte_slice){(const u8 *)response.bytes.ptr, response.bytes.len});
        }
        return;
    default:
        break;
    }
    control.result.tag = CONNECTION_STEP_INVALID_INPUT;
    Close();
}
