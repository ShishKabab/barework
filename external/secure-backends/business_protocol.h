#ifndef SECURE_BUSINESS_PROTOCOL_H
#define SECURE_BUSINESS_PROTOCOL_H
#include "connection_protocol.h"
#include "http.h"
#include "http_response_message.h"

enum { BUSINESS_REQUEST = BUSINESS_NAMESPACE + 1, BUSINESS_RESULT, BUSINESS_QUERY_REQUEST, BUSINESS_FAILED };
enum { BUSINESS_ABI_VERSION = 3, BUSINESS_RESPONSE_LIMIT = HTTP_RESPONSE_LIMIT,
       BUSINESS_MEMORY_LIMIT = 2 * 1024 * 1024 };
typedef enum business_init_tag {
    BUSINESS_INIT_OK, BUSINESS_INIT_INVALID_ARGUMENT, BUSINESS_INIT_NO_SPACE,
} business_init_tag;
typedef struct business_init_result { u32 tag; wasm_slice input; } business_init_result;
typedef enum business_handle_tag {
    BUSINESS_HANDLE_OK, BUSINESS_HANDLE_INVALID_REQUEST, BUSINESS_HANDLE_INVALID_STATE,
    BUSINESS_HANDLE_NO_SPACE, BUSINESS_HANDLE_INVALID_RESPONSE,
} business_handle_tag;
typedef struct business_handle_result { u32 tag, type; wasm_slice output; } business_handle_result;
_Static_assert(sizeof(business_init_result) == 12, "business init ABI");
_Static_assert(sizeof(business_handle_result) == 16, "business handle ABI");
#endif
