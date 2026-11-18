#ifndef SECURE_CONNECTION_PROTOCOL_H
#define SECURE_CONNECTION_PROTOCOL_H
#include "types.h"

/* Fixed-width little-endian ABI; namespaces reserve 4096 values each. */
enum {
    CONNECTION_OPEN = 0x1001, CONNECTION_RECEIVE, CONNECTION_EOF,
    CONNECTION_SEND_COMPLETE, CONNECTION_CLOSE, CONNECTION_REQUEST, CONNECTION_CLOSED,
    CONNECTION_NEED_INPUT = 0x1010, CONNECTION_SEND_BYTES, CONNECTION_HTTP_REQUEST,
    CONNECTION_CLOSE_REQUEST, CONNECTION_RESPONSE, CONNECTION_BUSINESS_FAILED,
    CONNECTION_ROUTED_REQUEST,
    BROKER_NAMESPACE = 0x2000, SIGNING_NAMESPACE = 0x3000, BUSINESS_NAMESPACE = 0x4000,
};
enum { CONNECTION_ABI_VERSION = 4, CONNECTION_INPUT_SIZE = 4096,
       CONNECTION_MESSAGE_LIMIT = 128 * 1024, CONNECTION_MEMORY_LIMIT = 2 * 1024 * 1024,
       CONNECTION_DISPATCH_LIMIT = 256 * 1024,
       CONNECTION_MAX_ACTIONS = 3 };
enum { CONNECTION_WANT_INPUT = 1, CONNECTION_WANT_SEND = 2,
       CONNECTION_HAS_REQUEST = 4, CONNECTION_WANT_CLOSE = 8 };
typedef struct wasm_slice { u32 ptr; u32 len; } wasm_slice;
typedef enum connection_init_tag {
    CONNECTION_INIT_OK, CONNECTION_INIT_ALREADY_INITIALIZED,
    CONNECTION_INIT_UNSUPPORTED_VERSION, CONNECTION_INIT_NO_SPACE,
} connection_init_tag;
/* ConnectionInit returns an offset to this fixed-width tagged result. */
typedef struct connection_init_result { u32 tag; u32 control_offset; } connection_init_result;
typedef struct connection_action { u32 type; wasm_slice data; } connection_action;
typedef enum connection_step_tag {
    CONNECTION_STEP_OK, CONNECTION_STEP_INVALID_STATE, CONNECTION_STEP_INVALID_INPUT,
} connection_step_tag;
typedef struct connection_step_result { u32 tag; } connection_step_result;
typedef struct connection_control {
    u32 abi_version;
    u32 struct_size;
    wasm_slice input; /* bytes; fixed scratch storage outside request arena */
    u32 input_type;
    u32 input_length;
    u32 output_type;
    connection_step_result result;
    u32 consumed; /* Network input accepted, not HTTP parser consumption. */
    wasm_slice actions; /* len counts connection_action elements */
    wasm_slice response_input; /* Separate from buffered network/pipeline bytes. */
} connection_control;
_Static_assert(sizeof(connection_control) == 52, "connection ABI layout");
_Static_assert(sizeof(connection_action) == 12, "connection action layout");
_Static_assert(sizeof(connection_init_result) == 8, "connection init result layout");
_Static_assert(sizeof(connection_step_result) == 4, "connection step result layout");
#endif
